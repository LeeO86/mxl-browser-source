// SPDX-License-Identifier: MIT
// Paint buffers (SPEC §2.2): OnPaint copies CEF's buffer, valid only during the call, into one
// of three BGRA buffers and returns; only the newest complete paint is kept. The converter
// takes it. The lock guards the buffer roles only and is never held during a copy.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mbs::engine
{
    class FrameStore
    {
    public:
        FrameStore(std::uint32_t width, std::uint32_t height);

        /// UI thread. Copies one paint (`stride` bytes per row). False when the size differs
        /// (a resize in flight): the paint is dropped.
        bool paint(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height);

        struct Frame
        {
            std::uint8_t const* bgra = nullptr; // width × 4 bytes per row
            std::uint64_t sequence = 0;         // paints since construction, 1 for the first
        };

        /// Converter. The newest paint not taken yet, or no `bgra` when none arrived. The frame
        /// stays valid until the next take().
        Frame take();

        [[nodiscard]] std::uint32_t width() const { return _width; }
        [[nodiscard]] std::uint32_t height() const { return _height; }
        [[nodiscard]] std::uint64_t dropped() const; // paints replaced before they were taken

    private:
        std::uint32_t _width;
        std::uint32_t _height;
        std::vector<std::uint8_t> _buffers[3];
        mutable std::mutex _mutex;
        int _writing = 0; // the UI thread copies into this one
        int _ready = -1;  // newest complete paint, not taken yet
        int _reading = 1; // the converter holds this one
        std::uint64_t _readySequence = 0;
        std::uint64_t _sequence = 0;
        std::uint64_t _dropped = 0;
    };
}
