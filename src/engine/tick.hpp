// SPDX-License-Identifier: MIT
// The tick (SPEC §2.4, spike S2): once per grain index it commits the newest converted paint
// or repeats the last frame, writes the grain's audio, and sends the page one BeginFrame.
// A BeginFrame on an unchanged page produces no paint, so there is no "outstanding" state to
// wait for: every tick asks again. Pure logic: the caller supplies the clock and the actions,
// so unit tests run it without CEF or libmxl.
#pragma once

#include <cstdint>

namespace mbs::engine
{
    enum class PageState
    {
        Loading,
        Ready,
        Crashed,
        Hung,
    };

    class TickActions
    {
    public:
        virtual ~TickActions() = default;
        /// Writes grain `index`. Returns true when it carries a new frame, false for a repeat.
        virtual bool commit(std::uint64_t index, PageState state) = 0;
        /// Writes the audio samples of grain `index`.
        virtual void writeAudio(std::uint64_t index) = 0;
        /// Posts one BeginFrame to the UI thread.
        virtual void requestFrame() = 0;
    };

    struct TickCounters
    {
        std::uint64_t grains = 0;         // grains committed, repeats included
        std::uint64_t repeated = 0;       // ready page, no new paint (unchanged, or late)
        std::uint64_t repeatedHold = 0;   // repeats while the page loads, crashed or hangs
        std::uint64_t missed = 0;         // indexes the thread woke too late for (committed as repeats)
        std::uint64_t beginFrames = 0;
    };

    class Tick
    {
    public:
        explicit Tick(TickActions& actions)
            : _actions(actions)
        {
        }

        /// Runs for grain `index` (the current index when the thread woke). Indexes since the
        /// last run that were not handled are committed as repeats first.
        void run(std::uint64_t index, PageState state);

        [[nodiscard]] TickCounters const& counters() const { return _counters; }

    private:
        TickActions& _actions;
        TickCounters _counters;
        bool _started = false;
        std::uint64_t _last = 0;
    };
}
