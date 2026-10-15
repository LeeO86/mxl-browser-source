// SPDX-License-Identifier: MIT
// A ring of the newest diagnostic events that the hot threads (the SCHED_FIFO tick, CEF's audio
// capture and UI threads) write without a lock, and that a reader (an HTTP request) copies
// without stopping them: each slot is a seqlock. A writer claims an event number with one
// fetch_add; a slot that is being written, or that a newer event overwrote while it was copied,
// is left out of the copy. Nothing is allocated after construction.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

namespace mbs::util
{
    template <typename Event>
    class TraceRing
    {
        static_assert(std::is_trivially_copyable_v<Event>);
        static constexpr std::size_t kWords = (sizeof(Event) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

        struct Slot
        {
            std::atomic<std::uint64_t> seq{0}; // 2n+1 while event n is written, 2n+2 once it is complete
            std::array<std::atomic<std::uint64_t>, kWords> words{};
        };

    public:
        explicit TraceRing(std::size_t capacity)
            : _capacity(capacity)
            , _slots(std::make_unique<Slot[]>(capacity))
        {
        }

        [[nodiscard]] std::size_t capacity() const { return _capacity; }

        /// Any thread, lock-free.
        void record(Event const& event)
        {
            std::array<std::uint64_t, kWords> raw{};
            std::memcpy(raw.data(), &event, sizeof(Event));
            auto const n = _written.fetch_add(1, std::memory_order_relaxed);
            auto& slot = _slots[n % _capacity];
            slot.seq.store(2 * n + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            for (std::size_t w = 0; w < kWords; ++w)
            {
                slot.words[w].store(raw[w], std::memory_order_relaxed);
            }
            slot.seq.store(2 * n + 2, std::memory_order_release);
        }

        /// The complete events, oldest first. Never blocks a writer.
        [[nodiscard]] std::vector<Event> snapshot() const
        {
            std::vector<Event> out;
            auto const total = _written.load(std::memory_order_acquire);
            auto const first = total > _capacity ? total - _capacity : 0;
            out.reserve(static_cast<std::size_t>(total - first));
            for (auto n = first; n < total; ++n)
            {
                auto const& slot = _slots[n % _capacity];
                auto const before = slot.seq.load(std::memory_order_acquire);
                if (before != 2 * n + 2)
                {
                    continue; // still being written, or already overwritten by a newer event
                }
                std::array<std::uint64_t, kWords> raw{};
                for (std::size_t w = 0; w < kWords; ++w)
                {
                    raw[w] = slot.words[w].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (slot.seq.load(std::memory_order_relaxed) != before)
                {
                    continue; // overwritten while copied
                }
                Event event;
                std::memcpy(static_cast<void*>(&event), raw.data(), sizeof(Event)); // trivially copyable (asserted above)
                out.push_back(event);
            }
            return out;
        }

    private:
        std::size_t _capacity;
        std::unique_ptr<Slot[]> _slots;
        std::atomic<std::uint64_t> _written{0};
    };
}
