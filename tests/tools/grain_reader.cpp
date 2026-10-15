// SPDX-License-Identifier: MIT
// Test reader for the frame-accuracy and A/V tests (SPEC §17).
//
//   grain_reader counter --domain DIR --flow UUID --width W --seconds S [--out FILE]
//     Decodes counter.html's 32-bit bar code (top 16 lines, bit i = 60 px block) from every
//     grain and reports advances (+1), repeats (+0) and skips (> +1). The test clips
//     (tests/media/make_clips.sh) carry the same bar code with the video frame number. --out
//     writes "grain_index value" per grain.
//   grain_reader audio --domain DIR --flow UUID --seconds S [--tone 1000] [--out FILE.f32]
//     Records channel 1 of an audio flow carrying a continuous tone (tone.html, the test
//     clips) and reports gaps (1 ms windows below -40 dBFS inside the tone) and phase jumps of
//     the tone (> 10 degrees between 10 ms blocks without a gap: a splice). --out writes the
//     samples as raw float32.
//   grain_reader change --domain DIR --flow UUID --width W --x X --y Y --seconds S
//     Index of the first grain whose luma at (X, Y) differs from the head grain's ("CHANGE grain=").
//   grain_reader sample --domain DIR --flow UUID --width W --height H --x X --y Y
//     Y, Cb, Cr (and the v210a alpha) of one pixel of the newest grain.
//   grain_reader avsync --domain DIR --video UUID --audio UUID --width W --rate N/D --seconds S
//     Finds avsync.html's white flashes (grains) and tone bursts (channel 1) and prints the
//     offset of each burst against its flash (positive: audio late).
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::string arg(int argc, char** argv, char const* name, std::string const& fallback)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::string(argv[i]) == name)
            {
                return argv[i + 1];
            }
        }
        return fallback;
    }

    mxlFlowReader openReader(mxlInstance instance, std::string const& flow)
    {
        mxlFlowReader reader = nullptr;
        for (int attempt = 0; attempt < 50 && mxlCreateFlowReader(instance, flow.c_str(), nullptr, &reader) != MXL_STATUS_OK; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return reader;
    }

    std::uint64_t headIndex(mxlFlowReader reader)
    {
        mxlFlowInfo info{};
        return mxlFlowReaderGetInfo(reader, &info) == MXL_STATUS_OK ? info.runtime.headIndex : 0;
    }

    // Luma of pixel x in a v210 line.
    int luma(std::uint8_t const* line, int x)
    {
        std::uint32_t w[4];
        std::memcpy(w, line + static_cast<std::size_t>(x / 6) * 16, sizeof(w));
        int const s[6] = {static_cast<int>((w[0] >> 10) & 0x3ff), static_cast<int>(w[1] & 0x3ff), static_cast<int>((w[1] >> 20) & 0x3ff),
            static_cast<int>((w[2] >> 10) & 0x3ff), static_cast<int>(w[3] & 0x3ff), static_cast<int>((w[3] >> 20) & 0x3ff)};
        return s[x % 6];
    }

    // Y, Cb, Cr (10 bit) and, for v210a, the alpha (10 bit) of pixel (x, y) in the newest grain.
    int sample(mxlInstance instance, std::string const& flow, int width, int height, int x, int y)
    {
        auto* reader = openReader(instance, flow);
        if (reader == nullptr)
        {
            std::fprintf(stderr, "no reader for %s\n", flow.c_str());
            return 1;
        }
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto const index = headIndex(reader);
        if (mxlFlowReaderGetGrain(reader, index, 200'000'000, &info, &payload) != MXL_STATUS_OK || payload == nullptr)
        {
            std::fprintf(stderr, "no grain\n");
            return 1;
        }
        std::size_t const rowBytes = static_cast<std::size_t>((width + 47) / 48) * 128;
        auto const* line = payload + static_cast<std::size_t>(y) * rowBytes;
        std::uint32_t w[4];
        std::memcpy(w, line + static_cast<std::size_t>(x / 6) * 16, sizeof(w));
        // Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5: chroma of the pixel pair
        int const pair = (x % 6) / 2;
        int const cbs[3] = {static_cast<int>(w[0] & 0x3ff), static_cast<int>((w[1] >> 10) & 0x3ff), static_cast<int>((w[2] >> 20) & 0x3ff)};
        int const crs[3] = {static_cast<int>((w[0] >> 20) & 0x3ff), static_cast<int>(w[2] & 0x3ff), static_cast<int>((w[3] >> 10) & 0x3ff)};
        std::printf("x=%d y=%d Y=%d Cb=%d Cr=%d", x, y, luma(line, x), cbs[pair], crs[pair]);
        // v210a: the alpha plane follows the fill (three 10-bit samples per word).
        std::size_t const alphaRow = static_cast<std::size_t>((width + 2) / 3) * 4;
        std::size_t const fillBytes = rowBytes * static_cast<std::size_t>(height);
        if (info.grainSize >= fillBytes + alphaRow * static_cast<std::size_t>(height))
        {
            std::uint32_t a = 0;
            std::memcpy(&a, payload + fillBytes + static_cast<std::size_t>(y) * alphaRow + static_cast<std::size_t>(x / 3) * 4, 4);
            std::printf(" A=%u", (a >> (10 * (x % 3))) & 0x3ff);
        }
        std::printf("\n");
        mxlReleaseFlowReader(instance, reader);
        return 0;
    }

    // The head grain's whole payload (fill, and alpha in v210a) into a file: the lab compares a page
    // and the same content as an OGraf graphic byte for byte (docs/ograf.md §12).
    int dump(mxlInstance instance, std::string const& flow, std::string const& out)
    {
        auto* reader = openReader(instance, flow);
        if (reader == nullptr)
        {
            std::fprintf(stderr, "no reader for %s\n", flow.c_str());
            return 1;
        }
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowReaderGetGrain(reader, headIndex(reader), 200'000'000, &info, &payload) != MXL_STATUS_OK || payload == nullptr)
        {
            std::fprintf(stderr, "no grain\n");
            return 1;
        }
        std::FILE* file = std::fopen(out.c_str(), "wb");
        bool const ok = file != nullptr && std::fwrite(payload, 1, info.grainSize, file) == info.grainSize;
        if (file != nullptr)
        {
            std::fclose(file);
        }
        std::printf("dump %s bytes=%llu\n", ok ? "ok" : "failed", static_cast<unsigned long long>(info.grainSize));
        mxlReleaseFlowReader(instance, reader);
        return ok ? 0 : 1;
    }

    // The first grain after the head grain whose luma at (x, y) differs from the head grain's by
    // more than 200 codes (interaction latency, SPEC §2.5).
    int change(mxlInstance instance, std::string const& flow, int width, int x, int y, int seconds)
    {
        auto* reader = openReader(instance, flow);
        if (reader == nullptr)
        {
            std::fprintf(stderr, "no reader for %s\n", flow.c_str());
            return 1;
        }
        std::size_t const rowBytes = static_cast<std::size_t>((width + 47) / 48) * 128;
        std::uint64_t index = headIndex(reader);
        int first = -1;
        int rc = 1;
        auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end)
        {
            mxlGrainInfo info{};
            std::uint8_t* payload = nullptr;
            if (mxlFlowReaderGetGrain(reader, index, 200'000'000, &info, &payload) != MXL_STATUS_OK || payload == nullptr)
            {
                index = std::max(index + 1, headIndex(reader));
                continue;
            }
            int const value = luma(payload + static_cast<std::size_t>(y) * rowBytes, x);
            if (first < 0)
            {
                first = value;
            }
            else if (std::abs(value - first) > 200)
            {
                std::printf("CHANGE grain=%llu luma=%d->%d\n", static_cast<unsigned long long>(index), first, value);
                std::fflush(stdout);
                rc = 0;
                break;
            }
            ++index;
        }
        if (rc != 0)
        {
            std::printf("RESULT none\n");
        }
        mxlReleaseFlowReader(instance, reader);
        return rc;
    }

    int counter(mxlInstance instance, std::string const& flow, int width, int seconds, std::string const& outPath)
    {
        FILE* out = outPath.empty() ? nullptr : std::fopen(outPath.c_str(), "w");
        auto* reader = openReader(instance, flow);
        if (reader == nullptr)
        {
            std::fprintf(stderr, "no reader for %s\n", flow.c_str());
            return 1;
        }
        std::size_t const rowBytes = static_cast<std::size_t>((width + 47) / 48) * 128;
        std::uint64_t index = headIndex(reader);
        long long last = -1;
        std::uint64_t grains = 0, advances = 0, repeats = 0, skips = 0, unreadable = 0, backwards = 0;
        auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end)
        {
            mxlGrainInfo info{};
            std::uint8_t* payload = nullptr;
            if (mxlFlowReaderGetGrain(reader, index, 200'000'000, &info, &payload) != MXL_STATUS_OK || payload == nullptr)
            {
                ++unreadable;
                index = std::max(index + 1, headIndex(reader));
                continue;
            }
            std::uint32_t value = 0;
            auto const* line = payload + 8 * rowBytes; // middle of the 16 bar-code lines
            for (int bit = 0; bit < 32; ++bit)
            {
                if (luma(line, bit * 60 + 30) > 512)
                {
                    value |= 1U << bit;
                }
            }
            ++grains;
            if (out != nullptr)
            {
                std::fprintf(out, "%llu %u\n", static_cast<unsigned long long>(index), value);
            }
            if (last >= 0)
            {
                long long const step = static_cast<long long>(value) - last;
                if (step == 1)
                {
                    ++advances;
                }
                else if (step == 0)
                {
                    ++repeats;
                }
                else if (step > 1)
                {
                    ++skips;
                    std::printf("skip at grain %llu: %lld -> %u\n", static_cast<unsigned long long>(index), last, value);
                }
                else
                {
                    ++backwards;
                }
            }
            last = value;
            ++index;
        }
        std::printf("RESULT grains=%llu advances=%llu repeats=%llu skips=%llu backwards=%llu unreadable=%llu last_counter=%lld\n",
            static_cast<unsigned long long>(grains), static_cast<unsigned long long>(advances), static_cast<unsigned long long>(repeats),
            static_cast<unsigned long long>(skips), static_cast<unsigned long long>(backwards), static_cast<unsigned long long>(unreadable), last);
        if (out != nullptr)
        {
            std::fclose(out);
        }
        mxlReleaseFlowReader(instance, reader);
        return 0;
    }

    // Channel 1 of `count` samples ending at sample index `end` (exclusive); empty when they
    // are not available.
    std::vector<float> readSamples(mxlFlowReader reader, std::uint64_t end, std::size_t count)
    {
        mxlWrappedMultiBufferSlice slices{};
        if (mxlFlowReaderGetSamples(reader, end, count, 200'000'000, &slices) != MXL_STATUS_OK)
        {
            return {};
        }
        std::vector<float> out;
        out.reserve(count);
        for (auto const& fragment : slices.base.fragments)
        {
            auto const* data = static_cast<float const*>(fragment.pointer);
            out.insert(out.end(), data, data + fragment.size / sizeof(float));
        }
        return out;
    }

    double dbfs(double rms)
    {
        // Sine-referenced: a full-scale sine is 0 dBFS (AES17).
        return rms > 1e-9 ? 20.0 * std::log10(rms * std::sqrt(2.0)) : -180.0;
    }

    int audio(mxlInstance instance, std::string const& flow, int seconds, double tone, std::string const& outPath)
    {
        auto* reader = openReader(instance, flow);
        if (reader == nullptr)
        {
            std::fprintf(stderr, "no reader for %s\n", flow.c_str());
            return 1;
        }
        constexpr std::size_t kBlock = 480;
        auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::uint64_t const start = headIndex(reader);
        std::uint64_t next = start;
        std::vector<float> recorded;
        std::vector<bool> lost; // per block: the reader fell behind the ring (not the writer's gap)
        while (std::chrono::steady_clock::now() < end)
        {
            auto block = readSamples(reader, next + kBlock, kBlock);
            if (block.size() != kBlock)
            {
                if (headIndex(reader) > next + 4800)
                {
                    recorded.insert(recorded.end(), kBlock, 0.F);
                    lost.push_back(true);
                    next += kBlock;
                }
                continue;
            }
            recorded.insert(recorded.end(), block.begin(), block.end());
            lost.push_back(false);
            next += kBlock;
        }
        mxlReleaseFlowReader(instance, reader);
        if (!outPath.empty())
        {
            if (FILE* f = std::fopen(outPath.c_str(), "wb"))
            {
                std::fwrite(recorded.data(), sizeof(float), recorded.size(), f);
                std::fclose(f);
            }
        }

        // 1 ms windows: level, and the stretch where the tone is present.
        constexpr std::size_t kWindow = 48;
        std::size_t const windows = recorded.size() / kWindow;
        std::vector<double> level(windows);
        for (std::size_t w = 0; w < windows; ++w)
        {
            double sum = 0;
            for (std::size_t i = 0; i < kWindow; ++i)
            {
                double const v = recorded[w * kWindow + i];
                sum += v * v;
            }
            level[w] = dbfs(std::sqrt(sum / kWindow));
        }
        auto const isLost = [&](std::size_t w) { return lost[w * kWindow / kBlock]; };
        std::size_t first = windows;
        std::size_t last = 0;
        for (std::size_t w = 0; w < windows; ++w)
        {
            if (level[w] > -30.0 && !isLost(w))
            {
                first = std::min(first, w);
                last = w;
            }
        }
        std::size_t gaps = 0;
        std::size_t gapWindows = 0;
        std::size_t longest = 0;
        std::size_t lostWindows = 0;
        double levelSum = 0;
        std::size_t loud = 0;
        for (std::size_t w = first; w <= last && w < windows;)
        {
            if (isLost(w))
            {
                ++lostWindows;
                ++w;
                continue;
            }
            if (level[w] >= -40.0)
            {
                levelSum += level[w];
                ++loud;
                ++w;
                continue;
            }
            std::size_t run = 0;
            while (w + run <= last && level[w + run] < -40.0 && !isLost(w + run))
            {
                ++run;
            }
            ++gaps;
            gapWindows += run;
            longest = std::max(longest, run);
            auto const sample = start + w * kWindow;
            std::printf("gap at sample %llu (TAI %.3f s): %zu ms\n", static_cast<unsigned long long>(sample), static_cast<double>(sample) / 48000.0, run);
            w += run;
        }

        // 10 ms blocks: the tone's phase against a continuous reference (absolute sample index).
        // A splice (a skip, a resampler reset) moves the phase; the resampler's slow correction
        // (≤ 2000 ppm, ≤ 7 degrees per block) moves it smoothly.
        constexpr double kPi = 3.14159265358979323846;
        std::vector<double> phase;
        std::vector<bool> clean;
        for (std::size_t b = 0; (b + 1) * kBlock <= recorded.size(); ++b)
        {
            double i = 0;
            double q = 0;
            bool ok = !lost[b];
            for (std::size_t n = 0; n < kBlock; ++n)
            {
                auto const index = start + b * kBlock + n;
                double const arg = 2 * kPi * tone * static_cast<double>(index % 48000) / 48000.0;
                i += recorded[b * kBlock + n] * std::cos(arg);
                q += recorded[b * kBlock + n] * std::sin(arg);
            }
            for (std::size_t w = b * kBlock / kWindow; w < (b + 1) * kBlock / kWindow && w < windows; ++w)
            {
                ok = ok && level[w] >= -40.0 && w >= first && w <= last;
            }
            phase.push_back(std::atan2(q, i) * 180.0 / kPi);
            clean.push_back(ok);
        }
        std::size_t jumps = 0;
        std::vector<double> recent;
        for (std::size_t b = 1; b < phase.size(); ++b)
        {
            if (!clean[b] || !clean[b - 1])
            {
                recent.clear();
                continue;
            }
            double step = phase[b] - phase[b - 1];
            step -= 360.0 * std::round(step / 360.0);
            double expected = 0;
            if (!recent.empty())
            {
                auto sorted = recent;
                std::sort(sorted.begin(), sorted.end());
                expected = sorted[sorted.size() / 2];
            }
            double off = step - expected;
            off -= 360.0 * std::round(off / 360.0);
            if (recent.size() >= 5 && std::fabs(off) > 10.0)
            {
                ++jumps;
                auto const sample = start + b * kBlock;
                std::printf("phase jump at sample %llu (TAI %.3f s): %+.1f deg\n", static_cast<unsigned long long>(sample), static_cast<double>(sample) / 48000.0, off);
            }
            recent.push_back(step);
            if (recent.size() > 20)
            {
                recent.erase(recent.begin());
            }
        }
        double const toneMs = first < windows ? static_cast<double>(last - first + 1) : 0.0;
        std::printf("RESULT seconds=%.1f tone_ms=%.0f level_dbfs=%.1f gaps=%zu gap_ms=%zu longest_gap_ms=%zu phase_jumps=%zu reader_lost_ms=%zu\n",
            static_cast<double>(recorded.size()) / 48000.0, toneMs, loud > 0 ? levelSum / static_cast<double>(loud) : -180.0, gaps, gapWindows, longest, jumps,
            lostWindows);
        return 0;
    }

    int avsync(mxlInstance instance, std::string const& videoFlow, std::string const& audioFlow, int width, mxlRational rate, int seconds)
    {
        auto* video = openReader(instance, videoFlow);
        auto* audio = openReader(instance, audioFlow);
        if (video == nullptr || audio == nullptr)
        {
            std::fprintf(stderr, "no reader\n");
            return 1;
        }
        auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

        // MXL keeps about 200 ms: channel 1 is recorded while the video is read.
        std::uint64_t const audioStart = headIndex(audio);
        std::vector<float> recorded;
        std::thread recorder([&] {
            std::uint64_t next = audioStart;
            while (std::chrono::steady_clock::now() < end)
            {
                auto block = readSamples(audio, next + 480, 480);
                if (block.size() != 480)
                {
                    // Fell behind the ring or not written yet: pad with silence to keep indexes.
                    if (headIndex(audio) > next + 4800)
                    {
                        recorded.insert(recorded.end(), 480, 0.F);
                        next += 480;
                    }
                    continue;
                }
                recorded.insert(recorded.end(), block.begin(), block.end());
                next += 480;
            }
        });

        std::size_t const rowBytes = static_cast<std::size_t>((width + 47) / 48) * 128;
        std::uint64_t index = headIndex(video);
        bool wasWhite = false;
        std::vector<std::uint64_t> flashes; // TAI ns of grains that turned white
        while (std::chrono::steady_clock::now() < end)
        {
            mxlGrainInfo info{};
            std::uint8_t* payload = nullptr;
            if (mxlFlowReaderGetGrain(video, index, 200'000'000, &info, &payload) != MXL_STATUS_OK || payload == nullptr)
            {
                index = std::max(index + 1, headIndex(video));
                continue;
            }
            std::size_t const height = info.grainSize / rowBytes;
            bool const white = luma(payload + (height / 2) * rowBytes, width / 2) > 800;
            if (white && !wasWhite)
            {
                flashes.push_back(mxlIndexToTimestamp(&rate, index));
            }
            wasWhite = white;
            ++index;
        }
        recorder.join();
        float peak = 0.F;
        for (float v : recorded)
        {
            peak = std::max(peak, std::fabs(v));
        }
        std::printf("audio recorded=%zu samples from %llu, peak %.3f\n", recorded.size(), static_cast<unsigned long long>(audioStart), static_cast<double>(peak));

        // Each flash: the burst onset within ±500 ms.
        std::vector<double> offsets;
        for (auto const flash : flashes)
        {
            auto const flashSample = static_cast<long long>(static_cast<double>(flash) * 48000.0 / 1e9);
            long long const from = std::max(0LL, flashSample - 24000 - static_cast<long long>(audioStart));
            long long const to = std::min(static_cast<long long>(recorded.size()), flashSample + 24000 - static_cast<long long>(audioStart));
            if (std::getenv("GRAIN_READER_DEBUG") != nullptr)
            {
                long long loudest = from;
                for (long long i = from; i < to; ++i)
                {
                    if (std::fabs(recorded[static_cast<std::size_t>(i)]) > std::fabs(recorded[static_cast<std::size_t>(loudest)]))
                    {
                        loudest = i;
                    }
                }
                std::printf("flash %llu window [%lld, %lld) loudest %.3f at %+.1f ms\n", static_cast<unsigned long long>(flash), from, to,
                    to > from ? static_cast<double>(recorded[static_cast<std::size_t>(loudest)]) : 0.0,
                    static_cast<double>(loudest + static_cast<long long>(audioStart) - flashSample) / 48.0);
            }
            // The burst (0.5 amplitude, 1 kHz) passes half its level within a twelfth of a period
            // of its start; the window (±500 ms) never reaches the previous burst (1 s earlier).
            for (long long i = from; i < to; ++i)
            {
                if (std::fabs(recorded[static_cast<std::size_t>(i)]) > 0.25F)
                {
                    offsets.push_back(static_cast<double>(i + static_cast<long long>(audioStart) - flashSample) / 48.0);
                    break;
                }
            }
        }
        double sum = 0;
        double lo = 1e9;
        double hi = -1e9;
        for (double o : offsets)
        {
            sum += o;
            lo = std::min(lo, o);
            hi = std::max(hi, o);
            std::printf("offset_ms %.2f\n", o);
        }
        std::printf("RESULT flashes=%zu measured=%zu mean_offset_ms=%.2f min=%.2f max=%.2f (positive: audio late)\n", flashes.size(), offsets.size(),
            offsets.empty() ? 0.0 : sum / static_cast<double>(offsets.size()), offsets.empty() ? 0.0 : lo, offsets.empty() ? 0.0 : hi);
        mxlReleaseFlowReader(instance, video);
        mxlReleaseFlowReader(instance, audio);
        return 0;
    }
}

int main(int argc, char** argv)
{
    std::string const mode = argc > 1 ? argv[1] : "";
    auto const domain = arg(argc, argv, "--domain", "");
    int const width = std::stoi(arg(argc, argv, "--width", "1920"));
    int const seconds = std::stoi(arg(argc, argv, "--seconds", "20"));
    if (domain.empty() || (mode != "counter" && mode != "avsync" && mode != "sample" && mode != "change" && mode != "dump" && mode != "audio"))
    {
        std::fprintf(stderr, "usage: grain_reader counter|audio|avsync|sample|change|dump --domain DIR (--flow UUID | --video UUID --audio UUID --rate 50/1) [--width 1920] [--seconds 20] [--tone 1000] [--out FILE]\n");
        return 2;
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (instance == nullptr)
    {
        std::fprintf(stderr, "no MXL instance for %s\n", domain.c_str());
        return 1;
    }
    int rc = 0;
    if (mode == "counter")
    {
        rc = counter(instance, arg(argc, argv, "--flow", ""), width, seconds, arg(argc, argv, "--out", ""));
    }
    else if (mode == "audio")
    {
        rc = audio(instance, arg(argc, argv, "--flow", ""), seconds, std::stod(arg(argc, argv, "--tone", "1000")), arg(argc, argv, "--out", ""));
    }
    else if (mode == "change")
    {
        rc = change(instance, arg(argc, argv, "--flow", ""), width, std::stoi(arg(argc, argv, "--x", "0")), std::stoi(arg(argc, argv, "--y", "0")), seconds);
    }
    else if (mode == "dump")
    {
        rc = dump(instance, arg(argc, argv, "--flow", ""), arg(argc, argv, "--out", "/tmp/grain.bin"));
    }
    else if (mode == "sample")
    {
        rc = sample(instance, arg(argc, argv, "--flow", ""), width, std::stoi(arg(argc, argv, "--height", "1080")), std::stoi(arg(argc, argv, "--x", "0")),
            std::stoi(arg(argc, argv, "--y", "0")));
    }
    else
    {
        auto const rateText = arg(argc, argv, "--rate", "50/1");
        mxlRational rate{std::stoll(rateText.substr(0, rateText.find('/'))), std::stoll(rateText.substr(rateText.find('/') + 1))};
        rc = avsync(instance, arg(argc, argv, "--video", ""), arg(argc, argv, "--audio", ""), width, rate, seconds);
    }
    mxlDestroyInstance(instance);
    return rc;
}
