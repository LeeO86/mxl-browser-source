// SPDX-License-Identifier: MIT
#include "convert/convert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace mbs::convert
{
    namespace
    {
        constexpr int kShift = 14;

        std::int32_t fixed(double v)
        {
            return static_cast<std::int32_t>(std::lround(v * (1 << kShift)));
        }

        std::int32_t clamp10(std::int32_t v)
        {
            return std::clamp(v, 4, 1019);
        }

        // (sum of three taps weighted 1 2 1, in 2^14 units) → rounded 10-bit chroma.
        std::int32_t chroma(std::int32_t left, std::int32_t centre, std::int32_t right)
        {
            std::int32_t const sum = left + 2 * centre + right; // ×4 ×2^14
            return clamp10(512 + ((sum + (1 << (kShift + 1))) >> (kShift + 2)));
        }

        void put(std::uint32_t* words, std::size_t index, std::uint32_t a, std::uint32_t b, std::uint32_t c)
        {
            words[index] = (a & 0x3FFu) | ((b & 0x3FFu) << 10) | ((c & 0x3FFu) << 20);
        }
    }

    void Row::resize(std::uint32_t width)
    {
        // Pass 1 may write up to 7 lanes beyond the row (AVX2 tail); size for it.
        std::size_t const n = static_cast<std::size_t>(width) + 8;
        y.resize(n);
        cb.resize(n);
        cr.resize(n);
        a.resize(n);
    }

    Coefficients const& coefficients()
    {
        static Coefficients const c = [] {
            double const kr = 0.2126;
            double const kb = 0.0722;
            double const kg = 1.0 - kr - kb;
            double const ys = 876.0 / 255.0;
            double const cs = 896.0 / 255.0;
            Coefficients out{};
            out.yR = fixed(ys * kr);
            out.yG = fixed(ys * kg);
            out.yB = fixed(ys * kb);
            // Cb = (B' − Y') / (2 (1 − Kb)),  Cr = (R' − Y') / (2 (1 − Kr))
            out.cbR = fixed(cs * -kr / (2.0 * (1.0 - kb)));
            out.cbG = fixed(cs * -kg / (2.0 * (1.0 - kb)));
            out.cbB = fixed(cs * 0.5);
            out.crR = fixed(cs * 0.5);
            out.crG = fixed(cs * -kg / (2.0 * (1.0 - kr)));
            out.crB = fixed(cs * -kb / (2.0 * (1.0 - kr)));
            return out;
        }();
        return c;
    }

    std::uint32_t const* unpremultiplyTable()
    {
        static std::array<std::uint32_t, 256> const table = [] {
            std::array<std::uint32_t, 256> t{};
            t[0] = 0; // alpha 0: colour 0
            for (std::uint32_t a = 1; a < 256; ++a)
            {
                t[a] = (255u * 65536u + a / 2u) / a;
            }
            return t;
        }();
        return table.data();
    }

    void pixelsScalar(std::uint8_t const* bgra, std::uint32_t count, bool straight, std::int32_t* y, std::int32_t* cb, std::int32_t* cr, std::uint8_t* a)
    {
        auto const& k = coefficients();
        auto const* table = unpremultiplyTable();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::int32_t b = bgra[4 * i];
            std::int32_t g = bgra[4 * i + 1];
            std::int32_t r = bgra[4 * i + 2];
            std::uint8_t const alpha = bgra[4 * i + 3];
            if (straight)
            {
                std::uint32_t const t = table[alpha];
                b = static_cast<std::int32_t>(std::min<std::uint32_t>(255u, (static_cast<std::uint32_t>(b) * t + 32768u) >> 16));
                g = static_cast<std::int32_t>(std::min<std::uint32_t>(255u, (static_cast<std::uint32_t>(g) * t + 32768u) >> 16));
                r = static_cast<std::int32_t>(std::min<std::uint32_t>(255u, (static_cast<std::uint32_t>(r) * t + 32768u) >> 16));
            }
            y[i] = clamp10(64 + ((k.yR * r + k.yG * g + k.yB * b + (1 << (kShift - 1))) >> kShift));
            cb[i] = k.cbR * r + k.cbG * g + k.cbB * b;
            cr[i] = k.crR * r + k.crG * g + k.crB * b;
            a[i] = alpha;
        }
    }

    void packFillRow(Row const& row, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        std::uint32_t const groups = (width + 5) / 6;
        auto const last = static_cast<std::int64_t>(width) - 1;
        auto const at = [&](std::vector<std::int32_t> const& v, std::int64_t x) { return v[static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, last))]; };
        auto const yAt = [&](std::uint32_t x) -> std::uint32_t { return x < width ? static_cast<std::uint32_t>(row.y[x]) : 0u; };
        auto const cAt = [&](std::vector<std::int32_t> const& v, std::uint32_t x) -> std::uint32_t {
            if (x >= width)
            {
                return 0u;
            }
            return static_cast<std::uint32_t>(chroma(at(v, static_cast<std::int64_t>(x) - 1), v[x], at(v, static_cast<std::int64_t>(x) + 1)));
        };
        for (std::uint32_t g = 0; g < groups; ++g)
        {
            std::uint32_t const x = g * 6;
            std::size_t const w = static_cast<std::size_t>(g) * 4;
            // Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5
            put(words, w + 0, cAt(row.cb, x), yAt(x), cAt(row.cr, x));
            put(words, w + 1, yAt(x + 1), cAt(row.cb, x + 2), yAt(x + 2));
            put(words, w + 2, cAt(row.cr, x + 2), yAt(x + 3), cAt(row.cb, x + 4));
            put(words, w + 3, yAt(x + 4), cAt(row.cr, x + 4), yAt(x + 5));
        }
        std::size_t const used = static_cast<std::size_t>(groups) * 16;
        std::memset(out + used, 0, v210RowBytes(width) - used);
    }

    void packAlphaRow(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        auto const a10 = [&](std::uint32_t x) -> std::uint32_t { return x < width ? (static_cast<std::uint32_t>(a[x]) * 1023u + 127u) / 255u : 0u; };
        std::uint32_t const n = (width + 2) / 3;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            put(words, i, a10(3 * i), a10(3 * i + 1), a10(3 * i + 2));
        }
    }

    void packKeyRow(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        auto const yk = [&](std::uint32_t x) -> std::uint32_t { return x < width ? 64u + (static_cast<std::uint32_t>(a[x]) * 876u + 127u) / 255u : 0u; };
        auto const ck = [&](std::uint32_t x) -> std::uint32_t { return x < width ? 512u : 0u; };
        std::uint32_t const groups = (width + 5) / 6;
        for (std::uint32_t g = 0; g < groups; ++g)
        {
            std::uint32_t const x = g * 6;
            std::size_t const w = static_cast<std::size_t>(g) * 4;
            put(words, w + 0, ck(x), yk(x), ck(x));
            put(words, w + 1, yk(x + 1), ck(x + 2), yk(x + 2));
            put(words, w + 2, ck(x + 2), yk(x + 3), ck(x + 4));
            put(words, w + 3, yk(x + 4), ck(x + 4), yk(x + 5));
        }
        std::size_t const used = static_cast<std::size_t>(groups) * 16;
        std::memset(out + used, 0, v210RowBytes(width) - used);
    }

    void convertFrame(Request const& req, PixelFn pixels)
    {
        if (pixels == nullptr)
        {
            pixels = avx2Available() ? &pixelsAvx2 : &pixelsScalar;
        }
        thread_local Row row;
        row.resize(req.width);
        std::size_t const fillStride = v210RowBytes(req.width);
        std::size_t const alphaStride = alphaRowBytes(req.width);
        for (std::uint32_t line = 0; line < req.height; ++line)
        {
            auto const* src = req.bgra + static_cast<std::size_t>(line) * req.bgraStride;
            pixels(src, req.width, req.straight, row.y.data(), row.cb.data(), row.cr.data(), row.a.data());
            packFillRow(row, req.width, req.fill + line * fillStride);
            if (req.alpha != nullptr)
            {
                packAlphaRow(row.a.data(), req.width, req.alpha + line * alphaStride);
            }
            if (req.key != nullptr)
            {
                packKeyRow(row.a.data(), req.width, req.key + line * fillStride);
            }
        }
    }

    void previewRgb(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height, std::uint32_t outWidth,
        std::uint32_t outHeight, std::uint8_t* rgb)
    {
        for (std::uint32_t oy = 0; oy < outHeight; ++oy)
        {
            std::uint32_t const y0 = oy * height / outHeight;
            std::uint32_t const y1 = std::max(y0 + 1, (oy + 1) * height / outHeight);
            for (std::uint32_t ox = 0; ox < outWidth; ++ox)
            {
                std::uint32_t const x0 = ox * width / outWidth;
                std::uint32_t const x1 = std::max(x0 + 1, (ox + 1) * width / outWidth);
                std::uint32_t sb = 0, sg = 0, sr = 0, sa = 0, n = 0;
                for (std::uint32_t y = y0; y < y1; ++y)
                {
                    auto const* p = bgra + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x0) * 4;
                    for (std::uint32_t x = x0; x < x1; ++x, p += 4)
                    {
                        sb += p[0];
                        sg += p[1];
                        sr += p[2];
                        sa += p[3];
                        ++n;
                    }
                }
                // Premultiplied over a checkerboard of 16-pixel squares (grey levels 102/153).
                std::uint32_t const checker = (((ox / 16) + (oy / 16)) & 1u) != 0 ? 153u : 102u;
                std::uint32_t const a = sa / n;
                auto* o = rgb + (static_cast<std::size_t>(oy) * outWidth + ox) * 3;
                o[0] = static_cast<std::uint8_t>(std::min(255u, sr / n + checker * (255u - a) / 255u));
                o[1] = static_cast<std::uint8_t>(std::min(255u, sg / n + checker * (255u - a) / 255u));
                o[2] = static_cast<std::uint8_t>(std::min(255u, sb / n + checker * (255u - a) / 255u));
            }
        }
    }
}
