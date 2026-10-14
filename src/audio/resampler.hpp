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

        Resampler(std::size_t channels, std::size_t targetFrames);
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

        /// A new fill target (BROWSER_AV_OFFSET_MS changed): the fill moves to it at once, with a
        /// stretch of silence or a cut.
        void setTarget(Fifo& fifo, std::size_t targetFrames);

        [[nodiscard]] ResamplerStats const& stats() const { return _stats; }
        [[nodiscard]] std::size_t target() const { return static_cast<std::size_t>(_target); }

    private:
        [[nodiscard]] std::size_t fill(Fifo const& fifo) const { return fifo.available() + _pending.size() / _channels; }
        void dropOldest(Fifo& fifo, std::size_t frames);
        void align(Fifo& fifo);
        std::size_t resample(Fifo& fifo, float* out, std::size_t frames, double ratio);

        std::size_t _channels;
        double _target;
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
