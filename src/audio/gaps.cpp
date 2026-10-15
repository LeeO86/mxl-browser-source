// SPDX-License-Identifier: MIT
#include "audio/gaps.hpp"

#include <cmath>

namespace mbs::audio
{
    std::size_t GapDetector::onPacket(std::int64_t ptsMs, std::size_t frames)
    {
        double const duration = static_cast<double>(frames) / kFramesPerMs;
        double const pts = static_cast<double>(ptsMs) + 0.5; // the middle of the truncated millisecond
        if (!_started)
        {
            _started = true;
            _expectedMs = pts + duration;
            return 0;
        }
        double const delta = pts - _expectedMs;
        if (delta > kToleranceMs)
        {
            ++_gaps;
            std::size_t fill = 0;
            if (delta <= kMaxFillMs)
            {
                // The loopback skips whole buffers: snap to a multiple of the packet when close.
                double const n = std::round(delta / duration);
                double const lost = n >= 1 && std::fabs(delta - n * duration) <= kToleranceMs ? n * duration : delta;
                fill = static_cast<std::size_t>(std::llround(lost * kFramesPerMs));
                _gapFrames += fill;
                _expectedMs += lost + duration;
            }
            else
            {
                ++_unfilled;
                _expectedMs = pts + duration;
            }
            return fill;
        }
        if (delta < -kToleranceMs)
        {
            _expectedMs = pts + duration; // overlapping or earlier than expected: follow the packets
            return 0;
        }
        // Follow the packet times slowly, so a rate difference between the stamps and the frame
        // count never adds up to a false gap.
        _expectedMs += duration + 0.05 * delta;
        return 0;
    }
}
