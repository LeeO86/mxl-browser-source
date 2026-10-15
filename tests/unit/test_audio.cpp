// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "audio/fifo.hpp"
#include "audio/gaps.hpp"
#include "audio/resampler.hpp"

using mbs::audio::Fifo;
using mbs::audio::Resampler;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // CEF's audio thread: 10 ms packets of a 1 kHz sine at 48 kHz × (1 + drift), planar stereo.
    struct Source
    {
        double drift = 0;
        double phase = 0;
        double clock = 0; // seconds of audio produced (in the source's own time)
        std::vector<float> left, right;

        // Delivers every packet completed by TAI `now`; the source produces 48000 × (1 + drift)
        // frames per second of TAI.
        void pushUntil(Fifo& fifo, double now)
        {
            while (clock + 0.01 <= now * (1 + drift))
            {
                left.resize(480);
                right.resize(480);
                for (std::size_t i = 0; i < 480; ++i)
                {
                    left[i] = right[i] = static_cast<float>(0.25 * std::sin(phase));
                    phase += 2 * kPi * 1000.0 / 48000.0;
                }
                float const* planes[2] = {left.data(), right.data()};
                clock += 0.01;
                fifo.push(planes, 2, 480, ns(clock / (1 + drift)));
            }
        }

        static std::uint64_t ns(double seconds) { return static_cast<std::uint64_t>(seconds * 1e9); }
    };
}

TEST_CASE("audio fifo interleaves planar packets and drops what does not fit")
{
    Fifo fifo(2, 4);
    float const l[3] = {1, 2, 3};
    float const r[3] = {-1, -2, -3};
    float const* planes[2] = {l, r};
    CHECK(fifo.push(planes, 2, 3, 0) == 3);
    CHECK(fifo.push(planes, 2, 3, 0) == 1);
    CHECK(fifo.available() == 4);
    float out[8] = {};
    CHECK(fifo.pop(out, 2) == 2);
    CHECK(out[0] == 1);
    CHECK(out[1] == -1);
    CHECK(out[2] == 2);
    CHECK(out[3] == -2);
    CHECK(fifo.drop(1) == 1);
    CHECK(fifo.pop(out, 8) == 1);
    CHECK(out[0] == 1);
    // A missing plane is silence.
    float const* mono[1] = {l};
    CHECK(fifo.push(mono, 1, 2, 0) == 2);
    CHECK(fifo.pop(out, 2) == 2);
    CHECK(out[1] == 0);
    CHECK(out[3] == 0);
}

TEST_CASE("resampler starts at the target, resumes after an underrun without priming, and cuts an overrun")
{
    Fifo fifo(2, 48000);
    Resampler resampler(2, 2880);
    Source source;
    std::vector<float> out(960 * 2);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(out[0] == 0);
    CHECK(resampler.stats().underruns == 0);
    // 100 ms arrived at once: the stream starts at the target, the oldest 40 ms are late.
    source.pushUntil(fifo, 0.1);
    resampler.produce(fifo, out.data(), 960, 0);
    double peak = 0;
    for (float const v : out)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }
    CHECK(peak > 0.2);
    // The source stalls: the FIFO runs dry once; no reset, no priming.
    for (int i = 0; i < 10; ++i)
    {
        resampler.produce(fifo, out.data(), 960, 0);
    }
    CHECK(resampler.stats().underruns == 1);
    CHECK(resampler.stats().resets == 0);
    auto const silence = resampler.stats().underrunFrames;
    CHECK(silence > 6 * 960);
    // The stalled 120 ms arrive late: what stood for them as silence is dropped, down to the
    // target, and the output resumes at once.
    source.pushUntil(fifo, 0.22);
    auto const queued = fifo.available();
    CHECK(queued > 5000);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(resampler.stats().overruns == 0);
    CHECK(resampler.stats().underruns == 1);
    CHECK(resampler.stats().realigned >= queued - 2880);
    CHECK(resampler.stats().realigned < queued - 2880 + 64); // plus what the resampler held
    CHECK(std::fabs(out[100]) > 0.0f);
    CHECK(static_cast<double>(fifo.available()) / 48000.0 < 0.061);
    // Running, far too much queued: what is still owed is dropped first, then cut back to the target.
    source.pushUntil(fifo, 0.8);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(resampler.stats().overruns == 1);
    CHECK(resampler.stats().resets == 1);
    CHECK(resampler.stats().fillSeconds < 0.07);
}

TEST_CASE("after an underrun the output resumes with one grain plus a packet, not the target")
{
    Fifo fifo(2, 48000);
    Resampler resampler(2, 2880);
    Source source;
    std::vector<float> out(960 * 2);
    double now = 0;
    auto tick = [&](bool feed) {
        now += 0.02;
        if (feed)
        {
            source.pushUntil(fifo, now);
        }
        else
        {
            source.clock = now; // a loss: those packets never come
        }
        resampler.produce(fifo, out.data(), 960, Source::ns(now));
        float p = 0;
        for (float const v : out)
        {
            p = std::max(p, std::fabs(v));
        }
        return p;
    };
    for (int i = 0; i < 50; ++i)
    {
        tick(true);
    }
    // 100 ms lost upstream without a trace in the packet times: the FIFO runs dry.
    for (int i = 0; i < 5; ++i)
    {
        tick(false);
    }
    CHECK(resampler.stats().underruns == 1);
    // Sound again after 2 ticks (40 ms ≥ 960 + 480 + 64 frames), not after 3 (60 ms).
    int silent = 0;
    while (tick(true) < 0.2f && silent < 10)
    {
        ++silent;
    }
    CHECK(silent == 1);
    CHECK(resampler.stats().underruns == 1);
    CHECK(resampler.stats().resets == 0);
    // The debt expires after 2 s; the controller then brings the fill back to the target faster
    // than ±500 ppm would.
    for (int i = 0; i < 1500; ++i) // 30 s
    {
        tick(true);
    }
    CHECK(resampler.stats().underruns == 1);
    CHECK(std::fabs(resampler.stats().fillSeconds - 0.06) < 0.01);
}

TEST_CASE("a stream that ends is played out, and a new one starts at the target without a cut")
{
    Fifo fifo(2, 48000);
    Resampler resampler(2, 2880);
    std::vector<float> out(960 * 2);
    std::vector<float> one(480, 0.5f);
    float const* planes[2] = {one.data(), one.data()};
    for (int i = 0; i < 4; ++i)
    {
        fifo.push(planes, 2, 480, 0);
    }
    resampler.produce(fifo, out.data(), 960, 0); // start: 1920 frames, 960 of silence first
    CHECK(out[0] == 0.f);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(out[2 * 959] == doctest::Approx(0.5f).epsilon(0.05));
    // The stream ends with 960 frames queued: they are played out, then silence; no underrun.
    CHECK_FALSE(resampler.drain(fifo, out.data(), 960));
    CHECK(out[2 * 400] == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(out[2 * 959] == 0.f);
    CHECK(resampler.stats().underruns == 0);
    // A new stream, 10 ms packets: its first packet plays one target after it arrived (two
    // ticks of silence), nothing is dropped and nothing runs dry.
    resampler.start();
    for (int t = 0; t < 3; ++t)
    {
        fifo.push(planes, 2, 480, 0);
        fifo.push(planes, 2, 480, 0);
        resampler.produce(fifo, out.data(), 960, 0);
        if (t < 2)
        {
            CHECK(out[2 * 959] == 0.f);
        }
        else
        {
            CHECK(out[2 * 959] == doctest::Approx(0.5f).epsilon(0.05));
        }
    }
    CHECK(resampler.stats().realigned == 0);
    CHECK(resampler.stats().underruns == 0);
}

TEST_CASE("gap detector: a jump in the packet times is filled with exactly the lost length")
{
    mbs::audio::GapDetector gaps;
    CHECK(gaps.onPacket(1000, 480) == 0);
    // Truncated milliseconds jitter by one: not a gap.
    CHECK(gaps.onPacket(1010, 480) == 0);
    CHECK(gaps.onPacket(1021, 480) == 0);
    CHECK(gaps.onPacket(1030, 480) == 0);
    // Three packets skipped by the loopback (30 ms): 1440 frames, snapped to whole packets.
    CHECK(gaps.onPacket(1070, 480) == 1440);
    CHECK(gaps.onPacket(1080, 480) == 0);
    // An odd jump is filled by its length in milliseconds (the stamps are whole milliseconds).
    auto const odd = gaps.onPacket(1097, 480);
    CHECK(odd >= 6 * 48);
    CHECK(odd <= 8 * 48);
    CHECK(gaps.gaps() == 2);
    CHECK(gaps.gapFrames() == 1440 + odd);
    // Far jumps are counted, not filled; earlier packets just restart the timeline.
    CHECK(gaps.onPacket(5000, 480) == 0);
    CHECK(gaps.unfilled() == 1);
    CHECK(gaps.onPacket(4000, 480) == 0);
    CHECK(gaps.onPacket(4010, 480) == 0);
    // A new stream does not count the time since the last one.
    gaps.reset();
    CHECK(gaps.onPacket(9000, 480) == 0);
    CHECK(gaps.gaps() == 3);
}

TEST_CASE("gap detector: packet times that run at a slightly different rate never make a gap")
{
    mbs::audio::GapDetector gaps;
    double t = 1e6;
    std::size_t filled = 0;
    for (int i = 0; i < 360000; ++i) // one hour, the stamps 300 ppm fast
    {
        filled += gaps.onPacket(static_cast<std::int64_t>(t), 480);
        t += 10.0 * (1 + 300e-6);
    }
    CHECK(filled == 0);
    CHECK(gaps.gaps() == 0);
}

TEST_CASE("resampler follows a drifting page clock without underruns")
{
    for (double const drift : {200e-6, -200e-6})
    {
        CAPTURE(drift);
        Fifo fifo(2, 48000);
        Resampler resampler(2, 2880); // 60 ms
        Source source;
        source.drift = drift;
        std::vector<float> out(960 * 2);
        double now = 0;
        float last = 0;
        double worstStep = 0;
        bool started = false;
        double ppmSum = 0;
        int ppmCount = 0;
        for (int tick = 0; tick < 15000; ++tick) // 300 s of 20 ms grains
        {
            now += 0.02;
            source.pushUntil(fifo, now);
            resampler.produce(fifo, out.data(), 960, Source::ns(now));
            if (tick >= 12500) // the last 50 s
            {
                ppmSum += resampler.stats().driftPpm;
                ++ppmCount;
            }
            for (std::size_t i = 0; i < 960; ++i)
            {
                if (started)
                {
                    worstStep = std::max(worstStep, static_cast<double>(std::fabs(out[i * 2] - last)));
                }
                last = out[i * 2];
                started = started || out[i * 2] != 0;
            }
        }
        auto const& stats = resampler.stats();
        CHECK(stats.underruns == 0);
        CHECK(stats.overruns == 0);
        CHECK(std::fabs(stats.fillSeconds - 0.06) < 0.005);
        CHECK(std::fabs(ppmSum / ppmCount - drift * 1e6) < 30);
        // A 1 kHz sine at 0.25 moves at most 0.25 × 2π × 1000 / 48000 ≈ 0.033 per sample.
        CHECK(worstStep < 0.04);
    }
}

namespace
{
    // Chromium's late bursts (docs/performance.md, M7): the source keeps producing, but nothing
    // is delivered for `stallSeconds`; then everything held arrives at once with its own times.
    struct StallRun
    {
        Fifo fifo{2, 48000};
        Resampler resampler;
        Source source;
        std::vector<float> out = std::vector<float>(960 * 2);
        double now = 0;
        double heldUntil = -1;

        explicit StallRun(std::size_t maxTarget)
            : resampler(2, 2880, maxTarget)
        {
        }
        void tick()
        {
            now += 0.02;
            if (now >= heldUntil)
            {
                source.pushUntil(fifo, now);
            }
            resampler.produce(fifo, out.data(), 960, Source::ns(now));
        }
        void run(double seconds)
        {
            for (int i = 0; i < static_cast<int>(std::lround(seconds / 0.02)); ++i)
            {
                tick();
            }
        }
        void stall(double seconds)
        {
            heldUntil = now + seconds;
            run(seconds + 0.5);
        }
    };
}

TEST_CASE("adaptive target: a late burst raises the target by the silence played, keeps the late audio, and absorbs the next stall")
{
    StallRun fixed(0);
    StallRun adaptive(14400); // up to 300 ms
    for (auto* r : {&fixed, &adaptive})
    {
        r->run(5);
        CHECK(r->resampler.stats().underruns == 0);
        r->stall(0.2); // 200 ms without packets against about 60 ms in the FIFO
        CHECK(r->resampler.stats().underruns == 1);
    }
    // Fixed: the late audio is dropped down to 60 ms, so the same stall makes the same hole again.
    CHECK(fixed.resampler.stats().realigned > 4800);
    CHECK(fixed.resampler.target() == 2880);
    // Adaptive: the target rose by the silence played (about 140-160 ms) and the late audio was
    // kept (at most what the fill stood above the target before the stall is dropped).
    auto const silence = adaptive.resampler.stats().underrunFrames;
    CHECK(silence > 6000);
    CHECK(adaptive.resampler.target() == 2880 + silence);
    CHECK(adaptive.resampler.extraFrames() == doctest::Approx(static_cast<double>(silence)));
    CHECK(adaptive.resampler.stats().realigned < 480);
    CHECK(adaptive.resampler.stats().targetRaises >= 1);
    CHECK(adaptive.resampler.stats().targetSeconds == doctest::Approx((2880.0 + static_cast<double>(silence)) / 48000.0));

    for (auto* r : {&fixed, &adaptive})
    {
        r->run(3);
        r->stall(0.2);
    }
    CHECK(fixed.resampler.stats().underruns == 2);
    CHECK(adaptive.resampler.stats().underruns == 1);
    CHECK(adaptive.resampler.stats().underrunFrames == silence);
}

TEST_CASE("adaptive target: capped at the maximum, held for 10 s, then back to the base at 1 ms/s without an underrun")
{
    StallRun r(7200); // up to 150 ms
    r.run(5);
    r.stall(0.35);
    CHECK(r.resampler.stats().underruns == 1);
    CHECK(r.resampler.target() == 7200);
    // What came late beyond the maximum is dropped as before.
    CHECK(r.resampler.stats().realigned > 0);
    r.run(9);
    CHECK(r.resampler.target() == 7200);
    // 90 ms back at 1 ms/s: 90 s after the hold.
    r.run(46);
    CHECK(r.resampler.target() > 2880 + 48 * 40);
    CHECK(r.resampler.target() < 2880 + 48 * 50);
    // The fill (the real latency, which the video delay follows) keeps up with the falling target.
    CHECK(std::fabs(r.resampler.stats().fillSeconds - static_cast<double>(r.resampler.target()) / 48000.0) < 0.008);
    r.run(60);
    CHECK(r.resampler.target() == 2880);
    r.run(30);
    CHECK(r.resampler.stats().underruns == 1);
    CHECK(r.resampler.stats().overruns == 0);
    CHECK(std::fabs(r.resampler.stats().fillSeconds - 0.06) < 0.01);
    CHECK(std::fabs(r.resampler.stats().driftPpm) < 600);
}

TEST_CASE("adaptive target: a new base (BROWSER_AV_OFFSET_MS) keeps what underruns added")
{
    StallRun r(14400);
    r.run(5);
    r.stall(0.2);
    auto const extra = r.resampler.extraFrames();
    REQUIRE(extra > 6000);
    r.resampler.setTarget(r.fifo, 2880 + 960);
    CHECK(r.resampler.extraFrames() == doctest::Approx(extra));
    CHECK(r.resampler.target() == static_cast<std::size_t>(2880 + 960 + extra));
}
