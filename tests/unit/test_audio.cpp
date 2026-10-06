// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "audio/fifo.hpp"
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

TEST_CASE("resampler primes with silence, then counts an underrun and an overrun")
{
    Fifo fifo(2, 48000);
    Resampler resampler(2, 2880);
    Source source;
    std::vector<float> out(960 * 2);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(out[0] == 0);
    CHECK(resampler.stats().underruns == 0);
    source.pushUntil(fifo, 0.1);
    resampler.produce(fifo, out.data(), 960, 0);
    double peak = 0;
    for (float const v : out)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }
    CHECK(peak > 0.2);
    // The source stops: the FIFO runs dry and silence is inserted once, then priming.
    for (int i = 0; i < 10; ++i)
    {
        resampler.produce(fifo, out.data(), 960, 0);
    }
    CHECK(resampler.stats().underruns == 1);
    CHECK(resampler.stats().resets == 1);
    // Priming ends at the target exactly: 120 ms arrived at once, 60 ms are kept.
    source.pushUntil(fifo, 0.22);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(resampler.stats().overruns == 0);
    CHECK(static_cast<double>(fifo.available()) / 48000.0 < 0.061);
    // Running, far too much queued: cut back to the target.
    source.pushUntil(fifo, 0.8);
    resampler.produce(fifo, out.data(), 960, 0);
    CHECK(resampler.stats().overruns == 1);
    CHECK(resampler.stats().fillSeconds < 0.07);
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
