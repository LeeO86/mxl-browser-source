// SPDX-License-Identifier: MIT
#include "audio/fifo.hpp"

#include <algorithm>

namespace mbs::audio
{
    Fifo::Fifo(std::size_t channels, std::size_t capacityFrames)
        : _channels(channels)
        , _capacity(capacityFrames)
        , _data(channels * capacityFrames, 0.f)
    {
    }

    std::size_t Fifo::push(float const* const* planes, int planeCount, std::size_t frames, std::uint64_t nowNs)
    {
        auto const head = _head.load(std::memory_order_relaxed);
        auto const tail = _tail.load(std::memory_order_acquire);
        auto const n = std::min(frames, _capacity - (head - tail));
        for (std::size_t i = 0; i < n; ++i)
        {
            float* frame = &_data[((head + i) % _capacity) * _channels];
            for (std::size_t c = 0; c < _channels; ++c)
            {
                frame[c] = static_cast<int>(c) < planeCount && planes[c] != nullptr ? planes[c][i] : 0.f;
            }
        }
        _lastPushFrames.store(n, std::memory_order_relaxed);
        _lastPushNs.store(nowNs, std::memory_order_relaxed);
        _head.store(head + n, std::memory_order_release);
        return n;
    }

    std::size_t Fifo::pop(float* out, std::size_t frames)
    {
        auto const tail = _tail.load(std::memory_order_relaxed);
        auto const head = _head.load(std::memory_order_acquire);
        auto const n = std::min(frames, head - tail);
        for (std::size_t i = 0; i < n; ++i)
        {
            float const* frame = &_data[((tail + i) % _capacity) * _channels];
            std::copy(frame, frame + _channels, out + i * _channels);
        }
        _tail.store(tail + n, std::memory_order_release);
        return n;
    }

    std::size_t Fifo::drop(std::size_t frames)
    {
        auto const tail = _tail.load(std::memory_order_relaxed);
        auto const head = _head.load(std::memory_order_acquire);
        auto const n = std::min(frames, head - tail);
        _tail.store(tail + n, std::memory_order_release);
        return n;
    }

    std::size_t Fifo::available() const
    {
        return _head.load(std::memory_order_acquire) - _tail.load(std::memory_order_acquire);
    }
}
