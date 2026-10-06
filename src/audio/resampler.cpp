// SPDX-License-Identifier: MIT
#include "audio/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include <samplerate.h>

namespace mbs::audio
{
    namespace
    {
        // Correction per second of fill error, and per second² of its integral: Ki = Kp²/4 makes
        // the loop critically damped (time constant 2/Kp = 50 s); 200 ppm of drift peaks at
        // about 4 ms of fill error. The fill estimate is smoothed over about a second against
        // the jitter of packet arrival.
        constexpr double kKp = 0.04;
        constexpr double kKi = kKp * kKp / 4;
        constexpr double kSmoothSeconds = 1.0;
        // Above this multiple of the target the FIFO is cut back instead of steered.
        constexpr double kOverrunFactor = 3.0;
    }

    Resampler::Resampler(std::size_t channels, std::size_t targetFrames)
        : _channels(channels)
        , _target(static_cast<double>(targetFrames))
    {
        int error = 0;
        _state = src_new(SRC_SINC_MEDIUM_QUALITY, static_cast<int>(channels), &error);
        if (_state == nullptr)
        {
            throw std::runtime_error(std::string("libsamplerate: ") + src_strerror(error));
        }
    }

    Resampler::~Resampler()
    {
        src_delete(_state);
    }

    void Resampler::restart()
    {
        src_reset(_state);
        _pending.clear();
        _integral = 0;
        _smoothed = -1;
        _priming = true;
        ++_stats.resets;
    }

    void Resampler::reset(Fifo& fifo)
    {
        fifo.drop(fifo.available());
        restart();
        _stats.resets--; // a requested start-over is not an out-of-range reset
    }

    void Resampler::setTarget(Fifo& fifo, std::size_t targetFrames)
    {
        _target = static_cast<double>(targetFrames);
        reset(fifo);
    }

    void Resampler::produce(Fifo& fifo, float* out, std::size_t frames, std::uint64_t nowNs)
    {
        _stats.samples += frames;
        auto const pendingFrames = [&] { return _pending.size() / _channels; };
        double fill = static_cast<double>(fifo.available() + pendingFrames());
        if (_priming && fill >= _target)
        {
            // Start at the target exactly: what arrived beyond it (a packet, or a burst after the
            // page started its audio) would otherwise take the controller minutes at ±500 ppm.
            if (auto const excess = static_cast<std::size_t>(fill - _target); excess > 0)
            {
                fifo.drop(std::min(excess, fifo.available()));
                fill = static_cast<double>(fifo.available() + pendingFrames());
            }
            _priming = false;
        }
        if (!_priming && fill > kOverrunFactor * _target)
        {
            fifo.drop(static_cast<std::size_t>(fill - _target));
            ++_stats.overruns;
            restart();
            _priming = false;
            fill = static_cast<double>(fifo.available() + pendingFrames());
        }
        if (_priming)
        {
            _stats.fillSeconds = fill / kSampleRate;
            std::fill(out, out + frames * _channels, 0.f);
            return;
        }
        // Between packets the FIFO stands still while the page keeps producing: count what has
        // been produced since the last packet (up to one packet). Without that, the fill is a
        // one-packet sawtooth (10 ms from CEF) that beats with the tick.
        double estimate = fill;
        auto const lastPush = fifo.lastPushNs();
        if (lastPush > 0 && nowNs > lastPush)
        {
            estimate += std::min(static_cast<double>(fifo.lastPushFrames()), static_cast<double>(nowNs - lastPush) * kSampleRate / 1e9);
        }
        double const dt = static_cast<double>(frames) / kSampleRate;
        _smoothed = _smoothed < 0 ? estimate : _smoothed + (estimate - _smoothed) * std::min(1.0, dt / kSmoothSeconds);
        _stats.fillSeconds = _smoothed / kSampleRate;

        // u > 0 when the FIFO is too full: consume input faster (ratio = output / input < 1).
        double const error = (_smoothed - _target) / kSampleRate;
        double integral = _integral + error * dt;
        double u = kKp * error + kKi * integral;
        if (std::fabs(u) > kMaxCorrection)
        {
            u = std::copysign(kMaxCorrection, u);
            integral = _integral; // no wind-up while saturated
        }
        _integral = integral;
        _stats.driftPpm = u * 1e6;
        double const ratio = 1.0 / (1.0 + u);

        std::size_t produced = 0;
        while (produced < frames)
        {
            // Enough input for the rest of this tick, plus room for the filter's look-ahead.
            auto const want = static_cast<std::size_t>(std::ceil(static_cast<double>(frames - produced) / ratio)) + 64;
            if (pendingFrames() < want)
            {
                auto const missing = want - pendingFrames();
                _scratch.resize(missing * _channels);
                auto const got = fifo.pop(_scratch.data(), missing);
                _pending.insert(_pending.end(), _scratch.begin(), _scratch.begin() + static_cast<std::ptrdiff_t>(got * _channels));
            }
            SRC_DATA data{};
            data.data_in = _pending.data();
            data.input_frames = static_cast<long>(pendingFrames());
            data.data_out = out + produced * _channels;
            data.output_frames = static_cast<long>(frames - produced);
            data.src_ratio = ratio;
            data.end_of_input = 0;
            if (src_process(_state, &data) != 0)
            {
                break;
            }
            _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(data.input_frames_used * static_cast<long>(_channels)));
            produced += static_cast<std::size_t>(data.output_frames_gen);
            if (data.output_frames_gen == 0 && fifo.available() == 0)
            {
                break; // ran dry
            }
        }
        if (produced < frames)
        {
            std::fill(out + produced * _channels, out + frames * _channels, 0.f);
            ++_stats.underruns;
            restart();
        }
    }
}
