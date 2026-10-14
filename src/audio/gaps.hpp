// SPDX-License-Identifier: MIT
// Upstream losses from the packet times (SPEC §6). CEF stamps each packet with its capture time
// in whole milliseconds of CLOCK_MONOTONIC. Chromium's loopback stream skips ahead when its
// thread is late (services/audio/loopback_stream.cc), so a lost stretch shows up as a jump in
// those times. The engine then puts exactly the lost length of silence into the FIFO before the
// packet: the gap is as long as the loss, and the FIFO keeps its fill, so the loss does not
// become an underrun later. Producer side (CEF's audio thread), no locks.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mbs::audio
{
    class GapDetector
    {
    public:
        static constexpr double kFramesPerMs = 48.0;
        static constexpr double kToleranceMs = 2.0;   // pts are truncated to whole ms: ±1 ms is normal jitter
        static constexpr double kMaxFillMs = 500.0;   // longer jumps are not filled (the FIFO would overrun)

        /// A new stream: the next packet starts a new timeline.
        void reset() { _started = false; }

        /// One packet of `frames` frames stamped `ptsMs`. Returns the frames of silence that
        /// belong before it (0 when it follows the previous packet).
        std::size_t onPacket(std::int64_t ptsMs, std::size_t frames);

        [[nodiscard]] std::uint64_t gaps() const { return _gaps; }
        [[nodiscard]] std::uint64_t gapFrames() const { return _gapFrames; }    // silence inserted
        [[nodiscard]] std::uint64_t unfilled() const { return _unfilled; }     // jumps too long to fill

    private:
        bool _started = false;
        double _expectedMs = 0; // where the next packet should start (exact, not truncated)
        std::uint64_t _gaps = 0;
        std::uint64_t _gapFrames = 0;
        std::uint64_t _unfilled = 0;
    };
}
