// SPDX-License-Identifier: MIT
// The tick (SPEC §2.4): once per grain index it commits the newest converted paint or repeats
// the last frame, writes the grain's audio, and asks the page for the next frame with at most
// one BeginFrame outstanding. Pure logic: the caller supplies the clock and the actions, so
// unit tests run it without CEF or libmxl.
#pragma once

#include <atomic>
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
        /// Writes grain `index`: the newest converted frame when `fresh`, else the last one again.
        virtual void commit(std::uint64_t index, bool fresh) = 0;
        /// Writes the audio samples of grain `index`.
        virtual void writeAudio(std::uint64_t index) = 0;
        /// Posts one BeginFrame to the UI thread.
        virtual void requestFrame() = 0;
    };

    struct TickCounters
    {
        std::uint64_t grains = 0;          // grains committed, repeats included
        std::uint64_t repeatedLate = 0;    // repeats while a BeginFrame was outstanding
        std::uint64_t repeatedHold = 0;    // repeats while the page loads, crashed or hangs
        std::uint64_t missed = 0;          // indexes the thread woke too late for (committed as repeats)
        std::uint64_t beginFramesSkipped = 0;
    };

    class Tick
    {
    public:
        explicit Tick(TickActions& actions)
            : _actions(actions)
        {
        }

        /// The UI thread delivered a paint for the outstanding BeginFrame and the converter
        /// turned it into grain buffers (called from the converter side, before the next tick).
        void framePainted();

        /// Runs for grain `index` (the current index when the thread woke). Indexes since the
        /// last run that were not handled are committed as repeats first.
        void run(std::uint64_t index, PageState state);

        [[nodiscard]] TickCounters const& counters() const { return _counters; }
        [[nodiscard]] bool outstanding() const { return _outstanding; }

    private:
        TickActions& _actions;
        TickCounters _counters;
        std::atomic<bool> _painted{false}; // set by the converter side, taken by the tick
        bool _outstanding = false;
        bool _started = false;
        std::uint64_t _last = 0;
    };
}
