// SPDX-License-Identifier: MIT
// Test reader for the frame-accuracy and A/V tests (SPEC §17).
//
//   grain_reader counter --domain DIR --flow UUID --width W --seconds S
//     Decodes counter.html's 32-bit bar code (top 16 lines, bit i = 60 px block) from every
//     grain and reports advances (+1), repeats (+0) and skips (> +1).
//   grain_reader avsync --domain DIR --video UUID --audio UUID --width W --rate N/D --seconds S
//     Finds avsync.html's white flashes (grains) and tone bursts (channel 1) and prints the
//     offset of each burst against its flash (positive: audio late).
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    int counter(mxlInstance instance, std::string const& flow, int width, int seconds)
    {
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

        // Each flash: the first sample above -20 dBFS after 50 ms of quiet, within ±500 ms.
        std::vector<double> offsets;
        for (auto const flash : flashes)
        {
            auto const flashSample = static_cast<long long>(static_cast<double>(flash) * 48000.0 / 1e9);
            long long const from = std::max(0LL, flashSample - 24000 - static_cast<long long>(audioStart));
            long long const to = std::min(static_cast<long long>(recorded.size()), flashSample + 24000 - static_cast<long long>(audioStart));
            int quiet = 0;
            for (long long i = from; i < to; ++i)
            {
                float const v = std::fabs(recorded[static_cast<std::size_t>(i)]);
                if (v > 0.1F && quiet >= 2400)
                {
                    offsets.push_back(static_cast<double>(i + static_cast<long long>(audioStart) - flashSample) / 48.0);
                    break;
                }
                quiet = v < 0.01F ? quiet + 1 : 0;
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
    if (domain.empty() || (mode != "counter" && mode != "avsync"))
    {
        std::fprintf(stderr, "usage: grain_reader counter|avsync --domain DIR (--flow UUID | --video UUID --audio UUID --rate 50/1) [--width 1920] [--seconds 20]\n");
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
        rc = counter(instance, arg(argc, argv, "--flow", ""), width, seconds);
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
