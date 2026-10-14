// SPDX-License-Identifier: MIT
// The internal frame clock (SPEC §2.4, BROWSER_FRAME_CLOCK=internal): Chromium runs its own
// BeginFrame source at the house rate, and this class keeps its phase locked to TAI.
//
// CEF 144 sets the vsync timebase of the browser's compositor to base::TimeTicks::Now() whenever
// CefBrowserHost::SetWindowlessFrameRate() runs (CefRenderWidgetHostViewOSR::SetFrameRate), and
// viz's delay-based BeginFrame source ticks at timebase + n·interval from then on. So calling it
// at T(k) + φ puts every later BeginFrame at T(n) + φ. Chromium's clock is CLOCK_MONOTONIC, which
// on Linux runs at the same rate as CLOCK_TAI (adjtimex slews both; only steps separate them),
// so the phase holds until a step, a new compositor (a new renderer view) or a late anchor.
//
// Pure logic, no CEF: the tick thread polls it once per grain and gets the TAI time at which the
// UI thread should call SetWindowlessFrameRate; the UI thread reports when it did.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace mbs::engine
{
    enum class LockReason : std::uint8_t
    {
        Start, // the browser was created
        Load,  // a page loaded (a new renderer process gets a new view and its own timebase)
        Drift, // the phase error passed the threshold
    };
    inline constexpr std::array<char const*, 3> kLockReasons{"start", "load", "drift"};

    struct FrameClockStats
    {
        bool locked = false;
        std::array<std::uint64_t, 3> locks{}; // anchors made, by LockReason
        std::uint64_t lateAnchors = 0;        // the UI thread reached the anchor time too late (not anchored, tried again)
        std::int64_t anchorErrorNs = 0;       // the newest anchor: call time − target
        std::int64_t phaseErrorNs = 0;        // now: where Chromium's BeginFrames fall against T(n) + φ
    };

    class FrameClock
    {
    public:
        /// `periodNs` must be the exact period Chromium uses (1 s / rate in whole microseconds).
        FrameClock(std::uint64_t periodNs, std::uint64_t phaseNs, std::uint64_t thresholdNs);

        /// Asks for a new anchor at the next opportunity (any thread).
        void relock(LockReason reason);

        /// Tick thread, once per grain. `nextGrainNs` is T(k+1), `offsetNs` is TAI − CLOCK_MONOTONIC
        /// now. Returns the TAI time at which SetWindowlessFrameRate should run (T(k+1) + φ), or 0.
        /// At most one anchor is in flight.
        std::uint64_t poll(std::uint64_t nextGrainNs, std::int64_t offsetNs, std::uint64_t nowNs);

        /// UI thread: the anchor for `targetNs` ran at TAI `calledNs` (0: it was too late and did
        /// not run), with TAI − CLOCK_MONOTONIC = `offsetNs` then.
        void anchored(std::uint64_t targetNs, std::uint64_t calledNs, std::int64_t offsetNs);

        [[nodiscard]] FrameClockStats stats() const;
        [[nodiscard]] std::uint64_t phaseNs() const { return _phase; }

        /// Wraps a time difference into [−P/2, P/2).
        [[nodiscard]] std::int64_t wrap(std::int64_t ns) const;

    private:
        static constexpr std::uint64_t kInFlightTimeoutNs = 1'000'000'000; // an anchor that never reported (browser gone)

        std::uint64_t const _period;
        std::uint64_t const _phase;
        std::uint64_t const _threshold;

        mutable std::mutex _mutex;
        bool _pending = true; // the first anchor
        LockReason _pendingReason = LockReason::Start;
        bool _inFlight = false;
        LockReason _inFlightReason = LockReason::Start;
        std::uint64_t _inFlightSince = 0;
        std::uint64_t _target = 0;     // the newest anchor's target and call time (TAI)
        std::uint64_t _called = 0;
        std::int64_t _calledOffset = 0; // TAI − CLOCK_MONOTONIC at the call
        FrameClockStats _stats;
    };
}
