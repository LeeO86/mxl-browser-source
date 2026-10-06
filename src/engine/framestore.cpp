// SPDX-License-Identifier: MIT
#include "engine/framestore.hpp"

#include <cstring>

namespace mbs::engine
{
    FrameStore::FrameStore(std::uint32_t width, std::uint32_t height)
        : _width(width)
        , _height(height)
    {
        for (auto& buffer : _buffers)
        {
            buffer.assign(static_cast<std::size_t>(width) * height * 4, 0);
        }
    }

    bool FrameStore::paint(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height)
    {
        if (width != _width || height != _height)
        {
            return false;
        }
        int target = 0;
        {
            std::lock_guard lock{_mutex};
            target = _writing;
        }
        // Only the UI thread writes into `_writing`, so the copy runs unlocked.
        auto* out = _buffers[target].data();
        std::size_t const row = static_cast<std::size_t>(width) * 4;
        if (stride == row)
        {
            std::memcpy(out, bgra, row * height);
        }
        else
        {
            for (std::uint32_t y = 0; y < height; ++y)
            {
                std::memcpy(out + y * row, bgra + y * stride, row);
            }
        }
        std::lock_guard lock{_mutex};
        if (_ready >= 0)
        {
            ++_dropped;
            _writing = _ready; // the replaced paint's buffer is free again
        }
        else
        {
            _writing = 3 - target - _reading; // the buffer neither written nor read
        }
        _ready = target;
        _readySequence = ++_sequence;
        return true;
    }

    FrameStore::Frame FrameStore::take()
    {
        std::lock_guard lock{_mutex};
        if (_ready < 0)
        {
            return {};
        }
        std::swap(_reading, _ready);
        _ready = -1;
        return Frame{_buffers[_reading].data(), _readySequence};
    }

    std::uint64_t FrameStore::dropped() const
    {
        std::lock_guard lock{_mutex};
        return _dropped;
    }
}
