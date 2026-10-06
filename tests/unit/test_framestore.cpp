// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "engine/framestore.hpp"

using mbs::engine::FrameStore;

TEST_CASE("frame store keeps the newest paint and counts the replaced ones")
{
    FrameStore store(4, 2);
    CHECK(store.take().bgra == nullptr);
    std::vector<std::uint8_t> a(4 * 2 * 4, 1), b(4 * 2 * 4, 2);
    CHECK(store.paint(a.data(), 16, 4, 2));
    CHECK(store.paint(b.data(), 16, 4, 2));
    auto const frame = store.take();
    REQUIRE(frame.bgra != nullptr);
    CHECK(frame.bgra[0] == 2);
    CHECK(frame.sequence == 2);
    CHECK(store.dropped() == 1);
    CHECK(store.take().bgra == nullptr);
    // A paint of another size (a resize in flight) is dropped.
    std::vector<std::uint8_t> big(8 * 2 * 4, 3);
    CHECK_FALSE(store.paint(big.data(), 32, 8, 2));
    // Rows with padding are copied row by row.
    std::vector<std::uint8_t> padded(2 * 20, 0);
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            padded[static_cast<std::size_t>(y * 20 + x)] = static_cast<std::uint8_t>(10 + y);
        }
    }
    CHECK(store.paint(padded.data(), 20, 4, 2));
    auto const rows = store.take();
    CHECK(rows.bgra[0] == 10);
    CHECK(rows.bgra[15] == 10);
    CHECK(rows.bgra[16] == 11);
}

TEST_CASE("frame store never hands out a torn frame")
{
    constexpr std::uint32_t w = 256, h = 64;
    FrameStore store(w, h);
    std::atomic<bool> done{false};
    std::thread ui([&] {
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
        for (int i = 0; i < 3000; ++i)
        {
            std::fill(px.begin(), px.end(), static_cast<std::uint8_t>(i));
            store.paint(px.data(), w * 4, w, h);
        }
        done = true;
    });
    int taken = 0;
    bool uniform = true;
    std::uint64_t last = 0;
    bool increasing = true;
    while (!done || taken == 0)
    {
        auto const frame = store.take();
        if (frame.bgra == nullptr)
        {
            continue;
        }
        ++taken;
        increasing = increasing && frame.sequence > last;
        last = frame.sequence;
        for (std::size_t i = 1; i < static_cast<std::size_t>(w) * h * 4; ++i)
        {
            uniform = uniform && frame.bgra[i] == frame.bgra[0];
        }
    }
    ui.join();
    CHECK(taken > 0);
    CHECK(uniform);
    CHECK(increasing);
}
