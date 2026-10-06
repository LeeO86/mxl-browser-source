// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "engine/tick.hpp"

using mbs::engine::PageState;
using mbs::engine::Tick;

namespace
{
    // Commits a new frame when one was "painted" since the last commit and the page is ready,
    // as the engine does.
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
        bool painted = false;

        bool commit(std::uint64_t index, PageState state) override
        {
            bool const fresh = painted && state == PageState::Ready;
            if (fresh)
            {
                painted = false;
            }
            commits.push_back({index, fresh});
            return fresh;
        }
        void writeAudio(std::uint64_t index) override { audio.push_back(index); }
        void requestFrame() override { ++requests; }
    };
}

TEST_CASE("every tick commits one grain, writes its audio and sends one BeginFrame")
{
    Recorder actions;
    Tick tick(actions);
    tick.run(100, PageState::Ready); // nothing painted yet
    for (std::uint64_t k = 101; k < 111; ++k)
    {
        actions.painted = true;
        tick.run(k, PageState::Ready);
    }
    CHECK(actions.requests == 11);
    CHECK(tick.counters().beginFrames == 11);
    REQUIRE(actions.commits.size() == 11);
    CHECK_FALSE(actions.commits[0].fresh);
    for (std::size_t i = 1; i < actions.commits.size(); ++i)
    {
        CHECK(actions.commits[i].fresh);
    }
    CHECK(tick.counters().repeated == 1);
    CHECK(actions.audio.size() == 11);
}

TEST_CASE("an unchanged or late page repeats the frame but is still asked every tick")
{
    Recorder actions;
    Tick tick(actions);
    actions.painted = true;
    tick.run(1, PageState::Ready);
    // A static page answers BeginFrames with no paint: the frame repeats, and a later change
    // is still picked up because every tick sends a BeginFrame.
    for (std::uint64_t k = 2; k < 6; ++k)
    {
        tick.run(k, PageState::Ready);
    }
    CHECK(tick.counters().repeated == 4);
    CHECK(actions.requests == 5);
    actions.painted = true;
    tick.run(6, PageState::Ready);
    CHECK(actions.commits.back().fresh);
    CHECK(actions.requests == 6);
}

TEST_CASE("indexes the thread woke too late for are committed as repeats")
{
    Recorder actions;
    Tick tick(actions);
    tick.run(10, PageState::Ready);
    tick.run(14, PageState::Ready);
    CHECK(tick.counters().missed == 3);
    REQUIRE(actions.commits.size() == 5);
    CHECK(actions.commits[1].index == 11);
    CHECK(actions.commits[3].index == 13);
    CHECK(actions.commits[4].index == 14);
    CHECK(actions.audio.size() == 5);
    // Waking twice within one grain does nothing.
    tick.run(14, PageState::Ready);
    CHECK(actions.commits.size() == 5);
}

TEST_CASE("loading and crashed pages hold the frame; a crashed page gets no BeginFrame")
{
    Recorder actions;
    Tick tick(actions);
    actions.painted = true;
    tick.run(1, PageState::Loading);
    tick.run(2, PageState::Loading);
    CHECK(tick.counters().repeatedHold == 2);
    CHECK(actions.requests == 2);
    tick.run(3, PageState::Crashed);
    tick.run(4, PageState::Crashed);
    CHECK(actions.requests == 2);
    CHECK(tick.counters().repeatedHold == 4);
    // The paint kept while loading is committed once the page is ready.
    tick.run(5, PageState::Ready);
    CHECK(actions.commits.back().fresh);
    CHECK(actions.requests == 3);
}
