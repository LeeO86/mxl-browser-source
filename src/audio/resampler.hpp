// SPDX-License-Identifier: MIT
// Chromium's audio clock against TAI (SPEC §6): the tick thread takes exactly the samples of
// its grain through a variable-ratio resampler (libsamplerate). A PI controller holds the
// FIFO fill at the target by moving the ratio within ±500 ppm.
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
        std::uint64_t samples = 0;   // frames produced, silence included
        std::uint64_t underruns = 0; // ticks that inserted silence because the FIFO ran dry
        std::uint64_t overruns = 0;  // times the FIFO was cut back to the target
        std::uint64_t resets = 0;    // back to the target after an underrun or overrun
        double driftPpm = 0;         // the controller's correction
        double fillSeconds = 0;      // FIFO fill (plus the resampler's input), smoothed over about 1 s
    };

    class Resampler
    {
    public:
        static constexpr double kSampleRate = 48000.0;
        static constexpr double kMaxCorrection = 500e-6;

        Resampler(std::size_t channels, std::size_t targetFrames);
        ~Resampler();
        Resampler(Resampler const&) = delete;
        Resampler& operator=(Resampler const&) = delete;

        /// Writes `frames` interleaved frames to `out` for the tick at TAI `nowNs`. Silence while
        /// the FIFO fills up to the target (after construction, reset() or an underrun).
        void produce(Fifo& fifo, float* out, std::size_t frames, std::uint64_t nowNs);

        /// Starts over: drops what the FIFO holds and waits for the target fill again.
        void reset(Fifo& fifo);

        /// A new fill target (BROWSER_AV_OFFSET_MS changed); starts over like reset().
        void setTarget(Fifo& fifo, std::size_t targetFrames);

        [[nodiscard]] ResamplerStats const& stats() const { return _stats; }

    private:
        void restart();

        std::size_t _channels;
        double _target;
        SRC_STATE_tag* _state = nullptr;
        std::vector<float> _pending; // FIFO frames handed to libsamplerate, not yet consumed
        std::vector<float> _scratch;
        bool _priming = true;
        double _integral = 0;
        double _smoothed = -1; // fill in frames, smoothed; < 0 until the first running tick
        ResamplerStats _stats;
    };
}
