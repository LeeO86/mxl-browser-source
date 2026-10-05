// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "engine/tick.hpp"

using mbs::engine::PageState;
using mbs::engine::Tick;

namespace
{
    struct Recorder : mbs::engine::TickActions
    {
        struct Commit
        {
            std::uint64_t index;
            bool fresh;
        };
        std::vector<Commit> commits;
        std::vector<std::uint64_t> audio;
        int requests = 0;

        void commit(std::uint64_t index, bool fresh) override { commits.push_back({index, fresh}); }
        void writeAudio(std::uint64_t index) override { audio.push_back(index); }
        void requestFrame() override { ++requests; }
    };
}

TEST_CASE("a page that keeps up gets one BeginFrame per grain and every grain is fresh")
{
    Recorder actions;
    Tick tick(actions);
    tick.run(100, PageState::Ready); // nothing painted yet: hold, first request
    for (std::uint64_t k = 101; k < 111; ++k)
    {
        tick.framePainted();
        tick.run(k, PageState::Ready);
    }
    CHECK(actions.requests == 11);
    CHECK(actions.commits.size() == 11);
    for (std::size_t i = 1; i < actions.commits.size(); ++i)
    {
        CHECK(actions.commits[i].fresh);
    }
    CHECK(tick.counters().repeatedLate == 0);
    CHECK(tick.counters().beginFramesSkipped == 0);
    CHECK(actions.audio.size() == 11);
}

TEST_CASE("a late paint repeats one grain and skips one BeginFrame")
{
    Recorder actions;
    Tick tick(actions);
    tick.run(1, PageState::Ready);
    tick.framePainted();
    tick.run(2, PageState::Ready);
    // The paint for grain 3 is late: grain 3 repeats, no second BeginFrame is sent.
    tick.run(3, PageState::Ready);
    CHECK_FALSE(actions.commits.back().fresh);
    CHECK(tick.counters().repeatedLate == 1);
    CHECK(tick.counters().beginFramesSkipped == 1);
    CHECK(actions.requests == 2);
    // It arrives before grain 4: used there, and the page is asked again.
    tick.framePainted();
    tick.run(4, PageState::Ready);
    CHECK(actions.commits.back().fresh);
    CHECK(actions.requests == 3);
}

TEST_CASE("indexes the thread woke too late for are committed as repeats")
{
    Recorder actions;
    Tick tick(actions);
    tick.run(10, PageState::Ready);
    tick.framePainted();
    tick.run(14, PageState::Ready);
    CHECK(tick.counters().missed == 3);
    REQUIRE(actions.commits.size() == 5);
    CHECK(actions.commits[1].index == 11);
    CHECK(actions.commits[3].index == 13);
    CHECK_FALSE(actions.commits[2].fresh);
    CHECK(actions.commits[4].index == 14);
    CHECK(actions.commits[4].fresh);
    CHECK(actions.audio.size() == 5);
    // Waking twice within one grain does nothing.
    tick.run(14, PageState::Ready);
    CHECK(actions.commits.size() == 5);
}

TEST_CASE("loading and crashed pages hold the frame; a crash clears the outstanding BeginFrame")
{
    Recorder actions;
    Tick tick(actions);
    tick.run(1, PageState::Loading);
    tick.run(2, PageState::Loading);
    CHECK(tick.counters().repeatedHold == 2);
    tick.run(3, PageState::Crashed);
    CHECK_FALSE(tick.outstanding());
    tick.run(4, PageState::Crashed);
    CHECK(actions.requests == 1);
    tick.run(5, PageState::Ready);
    CHECK(actions.requests == 2);
}
