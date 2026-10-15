// SPDX-License-Identifier: MIT
// Chromium's audio clock against TAI (SPEC §6): the tick thread takes exactly the samples of
// its grain through a variable-ratio resampler (libsamplerate). A PI controller holds the
// FIFO fill at the target by moving the ratio within ±500 ppm (±2000 ppm while the fill is more
// than 20 ms off, after a loss or a start).
//
// Timeline rules (1.3.0): a stream starts at the target exactly (silence for what is missing,
// the oldest frames dropped when a burst brought more), so its first sample is neither delayed
// nor cut. When the FIFO runs dry, the rest of the tick is silence and output resumes as soon as
// the FIFO holds one grain plus a packet, not the whole target; the silence played is a debt,
// and audio that arrives late for it is dropped (only down to the target), so the hole is as
// long as the lateness and the latency comes back to the target. The resampler's state is kept.
//
// Adaptive target (1.3.0, M7): Chromium sometimes delivers its audio late in bursts (a pause of
// 80-350 ms, then the held packets at once with their original pts; docs/performance.md). With a
// maximum above the base target, every frame of silence an underrun plays raises the target by
// the same amount (up to the maximum), so the late audio is kept instead of dropped and the
// next stall of that size is absorbed. After kHoldSeconds without an underrun the target falls
// back towards the base at kDecayMsPerSecond, which the controller follows (1000 ppm). The
// engine delays the video by the same amount, so lip sync holds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/fifo.hpp"

struct SRC_STATE_tag;

namespace mbs::audio
{
    struct ResamplerStats
    {
        std::uint64_t samples = 0;        // frames produced, silence included
        std::uint64_t underruns = 0;      // times the FIFO ran dry while a stream played
        std::uint64_t underrunFrames = 0; // silence inserted for them (until the FIFO held one grain plus a packet)
        std::uint64_t overruns = 0;       // times the FIFO was cut back to the target
        std::uint64_t resets = 0;         // fill out of range, cut back to the target (an overrun)
        std::uint64_t realigned = 0;      // frames dropped because they arrived after their silence was played
        double driftPpm = 0;              // the controller's correction
        double fillSeconds = 0;           // FIFO fill (plus the resampler's input), smoothed over about 1 s
        double targetSeconds = 0;         // the fill target now (base plus what underruns added)
        std::uint64_t targetRaises = 0;   // underrun ticks that raised the target
    };

    class Resampler
    {
    public:
        static constexpr double kSampleRate = 48000.0;
        static constexpr double kMaxCorrection = 500e-6;
        static constexpr double kMaxCorrectionFar = 2000e-6; // while the fill is more than kFarSeconds off
        static constexpr double kFarSeconds = 0.020;
        static constexpr std::uint64_t kDebtNs = 2'000'000'000; // late audio older than this is not dropped any more
        static constexpr std::size_t kPacketFrames = 480;       // CEF delivers 10 ms packets (SPEC §6)
        static constexpr double kHoldSeconds = 10.0;            // adaptive target: no decay this long after an underrun
        static constexpr double kDecayMsPerSecond = 1.0;        // then the target falls this fast (1000 ppm)

        /// `maxTargetFrames` > `targetFrames` makes the target adaptive (above); 0 or less keeps it fixed.
        Resampler(std::size_t channels, std::size_t targetFrames, std::size_t maxTargetFrames = 0);
        ~Resampler();
        Resampler(Resampler const&) = delete;
        Resampler& operator=(Resampler const&) = delete;

        /// Writes `frames` interleaved frames to `out` for the tick at TAI `nowNs`.
        void produce(Fifo& fifo, float* out, std::size_t frames, std::uint64_t nowNs);

        /// The stream stopped: plays out what the FIFO still holds, then silence. Returns false
        /// once the FIFO is empty (nothing is counted as an underrun).
        bool drain(Fifo& fifo, float* out, std::size_t frames);

        /// A stream starts (or starts again): the next tick that finds audio aligns the fill to
        /// the target.
        void start();

        /// Starts over: drops what the FIFO holds and starts.
        void reset(Fifo& fifo);

        /// A new base target (BROWSER_AV_OFFSET_MS changed): the fill moves to it (plus what the
        /// adaptive target added) at once, with a stretch of silence or a cut.
        void setTarget(Fifo& fifo, std::size_t targetFrames);

        [[nodiscard]] ResamplerStats const& stats() const { return _stats; }
        [[nodiscard]] std::size_t target() const { return static_cast<std::size_t>(_target); }
        /// What the adaptive target adds to the base now (frames); 0 when it is fixed.
        [[nodiscard]] double extraFrames() const { return _target - _base; }

        /// Frames waiting: the FIFO plus the resampler's input.
        [[nodiscard]] std::size_t fill(Fifo const& fifo) const { return fifo.available() + _pending.size() / _channels; }

    private:
        void dropOldest(Fifo& fifo, std::size_t frames);
        void align(Fifo& fifo);
        void raise(std::size_t silenceFrames, std::uint64_t nowNs);
        std::size_t resample(Fifo& fifo, float* out, std::size_t frames, double ratio);

        std::size_t _channels;
        double _base;            // the configured target (plus BROWSER_AV_OFFSET_MS)
        double _maxExtra;        // how far the adaptive target may rise above the base
        double _target;          // base <= target <= base + maxExtra
        std::uint64_t _lastUnderrunNs = 0;
        SRC_STATE_tag* _state = nullptr;
        std::vector<float> _pending; // FIFO frames handed to libsamplerate, not yet consumed
        std::vector<float> _scratch;
        bool _starting = true;   // waiting for the first audio of a stream
        std::size_t _lead = 0;   // frames of silence before the FIFO's audio (a start below the target)
        bool _waiting = false;   // ran dry: silence until the FIFO holds one grain plus a packet
        std::size_t _debt = 0;   // silence played for audio that may still arrive
        std::uint64_t _debtSinceNs = 0;
        double _integral = 0;
        double _smoothed = -1;   // fill in frames, smoothed; < 0 until the first running tick
        double _ratio = 1.0;
        ResamplerStats _stats;
    };
}
