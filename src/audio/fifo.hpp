// SPDX-License-Identifier: MIT
// Page audio FIFO (SPEC §6): CEF's audio thread pushes planar packets, the tick thread
// pops interleaved frames. Single producer, single consumer, no locks.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mbs::audio
{
    class Fifo
    {
    public:
        Fifo(std::size_t channels, std::size_t capacityFrames);

        /// Producer. Appends `frames` frames of `channels` planes (CEF's layout; a missing
        /// plane is silence), received at TAI `nowNs`. Returns the frames stored; the rest did
        /// not fit and is dropped.
        std::size_t push(float const* const* planes, int planeCount, std::size_t frames, std::uint64_t nowNs);

        /// When the last packet arrived and how long it was (frames), for the fill estimate.
        [[nodiscard]] std::uint64_t lastPushNs() const { return _lastPushNs.load(std::memory_order_acquire); }
        [[nodiscard]] std::size_t lastPushFrames() const { return _lastPushFrames.load(std::memory_order_acquire); }

        /// Consumer. Moves up to `frames` interleaved frames to `out`; returns the count.
        std::size_t pop(float* out, std::size_t frames);

        /// Consumer. Drops up to `frames` frames; returns the count.
        std::size_t drop(std::size_t frames);

        /// Frames stored (exact for the consumer, a lower bound for the producer).
        [[nodiscard]] std::size_t available() const;

        [[nodiscard]] std::size_t channels() const { return _channels; }
        [[nodiscard]] std::size_t capacity() const { return _capacity; }

    private:
        std::size_t _channels;
        std::size_t _capacity;
        std::vector<float> _data;
        std::atomic<std::size_t> _head{0}; // frames written, owned by the producer
        std::atomic<std::size_t> _tail{0}; // frames read, owned by the consumer
        std::atomic<std::uint64_t> _lastPushNs{0};
        std::atomic<std::size_t> _lastPushFrames{0};
    };
}
