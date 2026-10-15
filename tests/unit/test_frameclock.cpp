// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "engine/frameclock.hpp"

using mbs::engine::FrameClock;
using mbs::engine::LockReason;

namespace
{
    constexpr std::uint64_t kP = 20'000'000; // 50 Hz
    constexpr std::uint64_t kPhase = 3'000'000;
    constexpr std::int64_t kOffset = 37'000'000'000LL; // TAI − CLOCK_MONOTONIC, any value
}

TEST_CASE("frame clock: the first anchor is at T(k+1) + φ, one at a time, and its error is the phase error")
{
    FrameClock clock(kP, kPhase, 1'000'000);
    CHECK_FALSE(clock.stats().locked);
    auto const target = clock.poll(101 * kP, kOffset, 100 * kP + 1'000'000);
    CHECK(target == 101 * kP + kPhase);
    CHECK(clock.poll(102 * kP, kOffset, 101 * kP + 1'000'000) == 0); // in flight
    clock.anchored(target, target + 40'000, kOffset);
    auto const s = clock.stats();
    CHECK(s.locked);
    CHECK(s.locks[static_cast<std::size_t>(LockReason::Start)] == 1);
    CHECK(s.anchorErrorNs == 40'000);
    CHECK(s.phaseErrorNs == 40'000);
    CHECK(clock.poll(103 * kP, kOffset, 102 * kP + 1'000'000) == 0); // locked, nothing to do
}

TEST_CASE("frame clock: a TAI step moves the phase; past the threshold it locks again")
{
    FrameClock clock(kP, kPhase, 1'000'000);
    auto const target = clock.poll(11 * kP, kOffset, 10 * kP);
    clock.anchored(target, target, kOffset);
    // 0.6 ms: measured, not acted on.
    CHECK(clock.poll(12 * kP, kOffset + 600'000, 11 * kP) == 0);
    CHECK(clock.stats().phaseErrorNs == 600'000);
    // 2.5 ms, staying: locked again once it was seen on kDriftPolls polls in a row.
    for (unsigned i = 1; i < FrameClock::kDriftPolls; ++i)
    {
        CHECK(clock.poll(13 * kP, kOffset + 2'500'000, 12 * kP) == 0);
    }
    auto const again = clock.poll(13 * kP, kOffset + 2'500'000, 12 * kP);
    CHECK(again == 13 * kP + kPhase);
    clock.anchored(again, again + 10'000, kOffset + 2'500'000);
    CHECK(clock.stats().locks[static_cast<std::size_t>(LockReason::Drift)] == 1);
    CHECK(clock.poll(14 * kP, kOffset + 2'500'000, 13 * kP) == 0);
    CHECK(clock.stats().phaseErrorNs == 10'000);
}

TEST_CASE("frame clock: the phase error wraps around the period")
{
    FrameClock clock(kP, kPhase, 1'000'000);
    CHECK(clock.wrap(static_cast<std::int64_t>(kP) - 100'000) == -100'000);
    CHECK(clock.wrap(-static_cast<std::int64_t>(kP) + 100'000) == 100'000);
    CHECK(clock.wrap(3 * static_cast<std::int64_t>(kP) + 5) == 5);
    // A step of a whole period leaves the BeginFrames where they were.
    auto const target = clock.poll(2 * kP, kOffset, kP);
    clock.anchored(target, target, kOffset);
    CHECK(clock.poll(3 * kP, kOffset + static_cast<std::int64_t>(kP), 2 * kP) == 0);
    CHECK(clock.stats().phaseErrorNs == 0);
}

TEST_CASE("frame clock: a late anchor is not counted as a lock and is tried again; a load locks again")
{
    FrameClock clock(kP, kPhase, 1'000'000);
    auto const first = clock.poll(2 * kP, kOffset, kP);
    clock.anchored(first, 0, kOffset); // the UI thread came too late
    CHECK(clock.stats().lateAnchors == 1);
    CHECK_FALSE(clock.stats().locked);
    auto const second = clock.poll(3 * kP, kOffset, 2 * kP);
    CHECK(second == 3 * kP + kPhase);
    clock.anchored(second, second + 1, kOffset);
    CHECK(clock.stats().locks[static_cast<std::size_t>(LockReason::Start)] == 1);
    clock.relock(LockReason::Load);
    auto const third = clock.poll(4 * kP, kOffset, 3 * kP);
    CHECK(third == 4 * kP + kPhase);
    clock.anchored(third, third, kOffset);
    CHECK(clock.stats().locks[static_cast<std::size_t>(LockReason::Load)] == 1);
}

TEST_CASE("frame clock: an anchor that never answers is given up after a second")
{
    FrameClock clock(kP, kPhase, 1'000'000);
    CHECK(clock.poll(2 * kP, kOffset, kP) != 0);
    CHECK(clock.poll(3 * kP, kOffset, 2 * kP) == 0);
    CHECK(clock.poll(60 * kP, kOffset, kP + 1'100'000'000) == 60 * kP + kPhase);
}

TEST_CASE("frame clock: one offset reading past the threshold (a preemption) does not relock")
{
    FrameClock clock(kP, kPhase, 2'000'000);
    auto const target = clock.poll(2 * kP, kOffset, kP);
    clock.anchored(target, target, kOffset);
    for (std::uint64_t k = 3; k < 30; ++k)
    {
        // Every fourth reading is 5 ms off, the others are right: never kDriftPolls in a row.
        auto const offset = k % 4 == 0 ? kOffset + 5'000'000 : kOffset;
        CHECK(clock.poll(k * kP, offset, (k - 1) * kP) == 0);
    }
    CHECK(clock.stats().locks[static_cast<std::size_t>(LockReason::Drift)] == 0);
}

TEST_CASE("frame clock: an anchor skipped before the browser exists is not late and is asked again")
{
    FrameClock clock(kP, kPhase, 1'000'000);
    for (std::uint64_t k = 2; k < 6; ++k)
    {
        auto const target = clock.poll(k * kP, kOffset, (k - 1) * kP);
        CHECK(target == k * kP + kPhase);
        clock.skipped();
    }
    CHECK(clock.stats().lateAnchors == 0);
    CHECK_FALSE(clock.stats().locked);
    auto const target = clock.poll(7 * kP, kOffset, 6 * kP);
    clock.anchored(target, target, kOffset);
    CHECK(clock.stats().locks[static_cast<std::size_t>(LockReason::Start)] == 1);
    CHECK(clock.stats().lateAnchors == 0);
}
