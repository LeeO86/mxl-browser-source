// SPDX-License-Identifier: MIT
#include "engine/frameclock.hpp"

#include <cstdlib>

namespace mbs::engine
{
    FrameClock::FrameClock(std::uint64_t periodNs, std::uint64_t phaseNs, std::uint64_t thresholdNs)
        : _period(periodNs)
        , _phase(phaseNs % periodNs)
        , _threshold(thresholdNs)
    {
    }

    std::int64_t FrameClock::wrap(std::int64_t ns) const
    {
        auto const p = static_cast<std::int64_t>(_period);
        auto r = ns % p;
        if (r < 0)
        {
            r += p;
        }
        return r >= p / 2 ? r - p : r;
    }

    void FrameClock::relock(LockReason reason)
    {
        std::lock_guard lock{_mutex};
        _pending = true;
        _pendingReason = reason;
    }

    std::uint64_t FrameClock::poll(std::uint64_t nextGrainNs, std::int64_t offsetNs, std::uint64_t nowNs)
    {
        std::lock_guard lock{_mutex};
        if (_stats.locked)
        {
            // Chromium's grid is fixed in CLOCK_MONOTONIC from the call on; in TAI it moves by the
            // change of the offset since then. Its distance from the wanted grid T(n) + φ:
            auto const anchor = static_cast<std::int64_t>(_called - _target);
            _stats.phaseErrorNs = wrap(anchor + (offsetNs - _calledOffset));
            // One reading past the threshold can be a preemption inside monotonicOffsetNs(); a real
            // step or slew stays.
            _overThreshold = static_cast<std::uint64_t>(std::llabs(_stats.phaseErrorNs)) > _threshold ? _overThreshold + 1 : 0;
            if (_overThreshold >= kDriftPolls && !_pending && !_inFlight)
            {
                _pending = true;
                _pendingReason = LockReason::Drift;
            }
        }
        if (_inFlight && nowNs > _inFlightSince + kInFlightTimeoutNs)
        {
            _inFlight = false; // never answered: try again
            if (!_pending)
            {
                _pending = true;
                _pendingReason = _inFlightReason;
            }
        }
        if (!_pending || _inFlight)
        {
            return 0;
        }
        _pending = false;
        _inFlight = true;
        _inFlightReason = _pendingReason;
        _inFlightSince = nowNs;
        return nextGrainNs + _phase;
    }

    void FrameClock::anchored(std::uint64_t targetNs, std::uint64_t calledNs, std::int64_t offsetNs)
    {
        std::lock_guard lock{_mutex};
        _inFlight = false;
        if (calledNs == 0)
        {
            ++_stats.lateAnchors;
            if (!_pending)
            {
                _pending = true;
                _pendingReason = _inFlightReason;
            }
            return;
        }
        _target = targetNs;
        _called = calledNs;
        _calledOffset = offsetNs;
        _overThreshold = 0;
        _stats.locked = true;
        _stats.anchorErrorNs = static_cast<std::int64_t>(calledNs - targetNs);
        _stats.phaseErrorNs = wrap(_stats.anchorErrorNs);
        ++_stats.locks[static_cast<std::size_t>(_inFlightReason)];
    }

    void FrameClock::skipped()
    {
        std::lock_guard lock{_mutex};
        _inFlight = false;
        if (!_pending)
        {
            _pending = true;
            _pendingReason = _inFlightReason;
        }
    }

    FrameClockStats FrameClock::stats() const
    {
        std::lock_guard lock{_mutex};
        return _stats;
    }
}
