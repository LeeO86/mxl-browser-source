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
        // The resampler's look-ahead (frames it wants beyond what it produces).
        constexpr std::size_t kLookAhead = 64;
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

    void Resampler::start()
    {
        _starting = true;
        _lead = 0;
        _waiting = false;
        _debt = 0;
        _integral = 0;
        _smoothed = -1;
    }

    void Resampler::reset(Fifo& fifo)
    {
        fifo.drop(fifo.available());
        _pending.clear();
        src_reset(_state);
        start();
    }

    void Resampler::setTarget(Fifo& fifo, std::size_t targetFrames)
    {
        _target = static_cast<double>(targetFrames);
        if (!_starting)
        {
            _lead = 0;
            align(fifo);
        }
    }

    void Resampler::dropOldest(Fifo& fifo, std::size_t frames)
    {
        // The frames handed to libsamplerate are the oldest.
        auto const fromPending = std::min(frames, _pending.size() / _channels);
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(fromPending * _channels));
        fifo.drop(frames - fromPending);
    }

    void Resampler::align(Fifo& fifo)
    {
        auto const now = fill(fifo);
        auto const target = static_cast<std::size_t>(_target);
        if (now > target)
        {
            dropOldest(fifo, now - target); // a burst: what is beyond the target is already late
        }
        else
        {
            _lead = target - now; // the first sample plays one target after it arrived
        }
        _integral = 0;
        _smoothed = -1;
    }

    std::size_t Resampler::resample(Fifo& fifo, float* out, std::size_t frames, double ratio)
    {
        std::size_t produced = 0;
        while (produced < frames)
        {
            // Enough input for the rest of this tick, plus room for the filter's look-ahead.
            auto const pendingFrames = _pending.size() / _channels;
            auto const want = static_cast<std::size_t>(std::ceil(static_cast<double>(frames - produced) / ratio)) + kLookAhead;
            if (pendingFrames < want)
            {
                auto const missing = want - pendingFrames;
                _scratch.resize(missing * _channels);
                auto const got = fifo.pop(_scratch.data(), missing);
                _pending.insert(_pending.end(), _scratch.begin(), _scratch.begin() + static_cast<std::ptrdiff_t>(got * _channels));
            }
            SRC_DATA data{};
            data.data_in = _pending.data();
            data.input_frames = static_cast<long>(_pending.size() / _channels);
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
        return produced;
    }

    bool Resampler::drain(Fifo& fifo, float* out, std::size_t frames)
    {
        _stats.samples += frames;
        auto const produced = fill(fifo) > 0 ? resample(fifo, out, frames, _ratio) : 0;
        std::fill(out + produced * _channels, out + frames * _channels, 0.f);
        _stats.fillSeconds = static_cast<double>(fill(fifo)) / kSampleRate;
        return produced == frames && fill(fifo) > 0;
    }

    void Resampler::produce(Fifo& fifo, float* out, std::size_t frames, std::uint64_t nowNs)
    {
        _stats.samples += frames;
        auto const silence = [&](std::size_t from) { std::fill(out + from * _channels, out + frames * _channels, 0.f); };

        if (_starting)
        {
            if (fill(fifo) == 0)
            {
                _stats.fillSeconds = 0;
                silence(0); // no audio of the new stream yet
                return;
            }
            align(fifo);
            _starting = false;
        }

        std::size_t produced = 0;
        if (_lead > 0)
        {
            produced = std::min(_lead, frames);
            silence(0);
            _lead -= produced;
            if (produced == frames)
            {
                _stats.fillSeconds = static_cast<double>(fill(fifo)) / kSampleRate;
                return;
            }
        }
        std::size_t const rest = frames - produced;

        auto current = fill(fifo);
        auto const target = static_cast<std::size_t>(_target);
        if (_debt > 0)
        {
            if (nowNs > _debtSinceNs + kDebtNs)
            {
                _debt = 0; // it was a loss, not lateness: the controller brings the fill back
            }
            else if (current > target)
            {
                // Audio that arrived after the silence that stood in for it.
                auto const late = std::min(_debt, current - target);
                dropOldest(fifo, late);
                _debt -= late;
                _stats.realigned += late;
                current -= late;
            }
        }
        if (_waiting)
        {
            if (current < rest + kPacketFrames + kLookAhead)
            {
                silence(produced);
                _stats.underrunFrames += rest;
                if (_debt == 0)
                {
                    _debtSinceNs = nowNs;
                }
                _debt += rest;
                _stats.fillSeconds = static_cast<double>(current) / kSampleRate;
                return;
            }
            _waiting = false;
        }
        if (static_cast<double>(current) > kOverrunFactor * _target)
        {
            dropOldest(fifo, current - target);
            ++_stats.overruns;
            ++_stats.resets;
            current = target;
            _integral = 0;
            _smoothed = -1;
        }

        // Between packets the FIFO stands still while the page keeps producing: count what has
        // been produced since the last packet (up to one packet). Without that, the fill is a
        // one-packet sawtooth (10 ms from CEF) that beats with the tick.
        double estimate = static_cast<double>(current);
        auto const lastPush = fifo.lastPushNs();
        if (lastPush > 0 && nowNs > lastPush)
        {
            estimate += std::min(static_cast<double>(fifo.lastPushFrames()), static_cast<double>(nowNs - lastPush) * kSampleRate / 1e9);
        }
        double const dt = static_cast<double>(rest) / kSampleRate;
        _smoothed = _smoothed < 0 ? estimate : _smoothed + (estimate - _smoothed) * std::min(1.0, dt / kSmoothSeconds);
        _stats.fillSeconds = _smoothed / kSampleRate;

        // u > 0 when the FIFO is too full: consume input faster (ratio = output / input < 1).
        double const error = (_smoothed - _target) / kSampleRate;
        double const bound = std::fabs(error) > kFarSeconds ? kMaxCorrectionFar : kMaxCorrection;
        double integral = _integral + error * dt;
        double u = kKp * error + kKi * integral;
        if (std::fabs(u) > bound)
        {
            u = std::copysign(bound, u);
            integral = _integral; // no wind-up while saturated
        }
        _integral = integral;
        _stats.driftPpm = u * 1e6;
        _ratio = 1.0 / (1.0 + u);

        produced += resample(fifo, out + produced * _channels, rest, _ratio);
        if (produced < frames)
        {
            // Ran dry. The rest is silence; output resumes when one grain plus a packet is back.
            silence(produced);
            ++_stats.underruns;
            _stats.underrunFrames += frames - produced;
            if (_debt == 0)
            {
                _debtSinceNs = nowNs;
            }
            _debt += frames - produced;
            _waiting = true;
        }
    }
}
