// SPDX-License-Identifier: MIT
// BGRA (premultiplied, from CEF) → v210 fill, v210a alpha plane, fill_key key (SPEC §5.2, §5.3).
//
// Two passes per row:
//   1. per pixel (scalar reference or AVX2): fill colour (straight via a reciprocal
//      table, or premultiplied as delivered), Y in 10 bit, Cb/Cr in 14-bit fixed point
//      before rounding, alpha;
//   2. shared: co-sited [1 2 1]/4 chroma filter, rounding, clamp to 4…1019, v210 packing.
// The AVX2 pass 1 must give the same integers as the scalar one; pass 2 is common code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mbs::convert
{
    /// v210 line length: ceil(width/48) × 128 bytes.
    constexpr std::uint32_t v210RowBytes(std::uint32_t width)
    {
        return ((width + 47U) / 48U) * 128U;
    }

    /// v210a alpha plane line length: three 10-bit samples per 32-bit word.
    constexpr std::uint32_t alphaRowBytes(std::uint32_t width)
    {
        return ((width + 2U) / 3U) * 4U;
    }

    struct Request
    {
        std::uint8_t const* bgra = nullptr; // premultiplied BGRA8
        std::size_t bgraStride = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool straight = false;          // un-premultiply the fill (keyed straight fill)
        std::uint8_t* fill = nullptr;   // v210, v210RowBytes(width) per line
        std::uint8_t* alpha = nullptr;  // optional v210a alpha plane, alphaRowBytes(width) per line
        std::uint8_t* key = nullptr;    // optional key as v210 luma (fill_key), v210RowBytes(width) per line
    };

    /// Pass 1 output for one row.
    struct Row
    {
        std::vector<std::int32_t> y;  // 10-bit luma, clamped to 4…1019
        std::vector<std::int32_t> cb; // (Cb−512) × 2^14, unrounded
        std::vector<std::int32_t> cr; // (Cr−512) × 2^14, unrounded
        std::vector<std::uint8_t> a;  // alpha 0…255
        void resize(std::uint32_t width);
    };

    using PixelFn = void (*)(std::uint8_t const* bgra, std::uint32_t count, bool straight, std::int32_t* y, std::int32_t* cb, std::int32_t* cr,
        std::uint8_t* a);

    void pixelsScalar(std::uint8_t const* bgra, std::uint32_t count, bool straight, std::int32_t* y, std::int32_t* cb, std::int32_t* cr, std::uint8_t* a);
    /// AVX2 pass 1; only call when avx2Available().
    void pixelsAvx2(std::uint8_t const* bgra, std::uint32_t count, bool straight, std::int32_t* y, std::int32_t* cb, std::int32_t* cr, std::uint8_t* a);
    [[nodiscard]] bool avx2Available();

    /// Pass 2.
    void packFillRow(Row const& row, std::uint32_t width, std::uint8_t* out);
    void packAlphaRow(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out);
    void packKeyRow(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out);

    /// Converts a frame with the given pass 1 (default: AVX2 when available).
    void convertFrame(Request const& request, PixelFn pixels = nullptr);

    /// Reciprocal table for un-premultiplying: c = min(255, (c * table[a] + 32768) >> 16).
    std::uint32_t const* unpremultiplyTable();

    /// Fixed-point BT.709 limited-range coefficients (×2^14), shared by both pass-1 versions.
    struct Coefficients
    {
        std::int32_t yR, yG, yB;    // Y10 = 64 + ((yR R + yG G + yB B + 8192) >> 14)
        std::int32_t cbR, cbG, cbB; // (Cb−512)·2^14 = cbR R + cbG G + cbB B
        std::int32_t crR, crG, crB;
    };
    Coefficients const& coefficients();

    /// Downscaled RGB24 preview over a checkerboard (box filter), for the UI.
    void previewRgb(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height, std::uint32_t outWidth,
        std::uint32_t outHeight, std::uint8_t* rgb);
}
