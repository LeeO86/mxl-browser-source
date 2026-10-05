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
        // Filtered chroma first, at full resolution (the co-sited even pixels are taken below):
        // the interior in plain loops without bounds checks (AVX2 when available), the
        // clamped edge taps only at both ends.
        thread_local std::vector<std::uint32_t> fcb;
        thread_local std::vector<std::uint32_t> fcr;
        fcb.resize(width);
        fcr.resize(width);
        std::int32_t const* cb = row.cb.data();
        std::int32_t const* cr = row.cr.data();
        auto const last = width - 1;
        auto const edge = [&](std::int32_t const* v, std::uint32_t x) {
            auto const left = v[x == 0 ? 0 : x - 1];
            auto const right = v[x + 1 > last ? last : x + 1];
            return static_cast<std::uint32_t>(chroma(left, v[x], right));
        };
        fcb[0] = edge(cb, 0);
        fcr[0] = edge(cr, 0);
        fcb[last] = edge(cb, last);
        fcr[last] = edge(cr, last);
        if (width > 2)
        {
            if (avx2Available())
            {
                chromaRowAvx2(cb, 1, last, fcb.data());
                chromaRowAvx2(cr, 1, last, fcr.data());
            }
            else
            {
                for (std::uint32_t x = 1; x < last; ++x)
                {
                    fcb[x] = static_cast<std::uint32_t>(chroma(cb[x - 1], cb[x], cb[x + 1]));
                    fcr[x] = static_cast<std::uint32_t>(chroma(cr[x - 1], cr[x], cr[x + 1]));
                }
            }
        }
        // Chroma site c is pixel 2c.
        auto const cb10 = [&](std::uint32_t c) { return fcb[2 * c]; };
        auto const cr10 = [&](std::uint32_t c) { return fcr[2 * c]; };
        std::int32_t const* y = row.y.data();
        std::uint32_t const fullGroups = width / 6; // all six pixels inside the width
        for (std::uint32_t g = 0; g < fullGroups; ++g)
        {
            std::uint32_t const x = g * 6;
            std::uint32_t const c = g * 3;
            std::uint32_t* w = words + static_cast<std::size_t>(g) * 4;
            // Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5
            w[0] = cb10(c) | (static_cast<std::uint32_t>(y[x]) << 10) | (cr10(c) << 20);
            w[1] = static_cast<std::uint32_t>(y[x + 1]) | (cb10(c + 1) << 10) | (static_cast<std::uint32_t>(y[x + 2]) << 20);
            w[2] = cr10(c + 1) | (static_cast<std::uint32_t>(y[x + 3]) << 10) | (cb10(c + 2) << 20);
            w[3] = static_cast<std::uint32_t>(y[x + 4]) | (cr10(c + 2) << 10) | (static_cast<std::uint32_t>(y[x + 5]) << 20);
        }
        if (fullGroups < groups) // the partial last group: samples past the width are 0
        {
            std::uint32_t const x = fullGroups * 6;
            std::uint32_t const c = fullGroups * 3;
            auto const yAt = [&](std::uint32_t i) -> std::uint32_t { return i < width ? static_cast<std::uint32_t>(y[i]) : 0u; };
            auto const cbAt = [&](std::uint32_t i) -> std::uint32_t { return 2 * i < width ? cb10(i) : 0u; };
            auto const crAt = [&](std::uint32_t i) -> std::uint32_t { return 2 * i < width ? cr10(i) : 0u; };
            std::size_t const w = static_cast<std::size_t>(fullGroups) * 4;
            put(words, w + 0, cbAt(c), yAt(x), crAt(c));
            put(words, w + 1, yAt(x + 1), cbAt(c + 1), yAt(x + 2));
            put(words, w + 2, crAt(c + 1), yAt(x + 3), cbAt(c + 2));
            put(words, w + 3, yAt(x + 4), crAt(c + 2), yAt(x + 5));
        }
        std::size_t const used = static_cast<std::size_t>(groups) * 16;
        std::memset(out + used, 0, v210RowBytes(width) - used);
    }

    namespace
    {
        // round(a·1023/255) for the v210a alpha plane, and the key as legal luma.
        std::array<std::uint32_t, 256> const& alpha10Table()
        {
            static std::array<std::uint32_t, 256> const table = [] {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t a = 0; a < 256; ++a)
                {
                    t[a] = (a * 1023u + 127u) / 255u;
                }
                return t;
            }();
            return table;
        }

        std::array<std::uint32_t, 256> const& keyLumaTable()
        {
            static std::array<std::uint32_t, 256> const table = [] {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t a = 0; a < 256; ++a)
                {
                    t[a] = 64u + (a * 876u + 127u) / 255u;
                }
                return t;
            }();
            return table;
        }
    }

    void packAlphaRow(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        auto const& t = alpha10Table();
        std::uint32_t const full = width / 3;
        for (std::uint32_t i = 0; i < full; ++i)
        {
            words[i] = t[a[3 * i]] | (t[a[3 * i + 1]] << 10) | (t[a[3 * i + 2]] << 20);
        }
        if (full * 3 < width)
        {
            std::uint32_t const x = full * 3;
            words[full] = t[a[x]] | ((x + 1 < width ? t[a[x + 1]] : 0u) << 10);
        }
    }

    void packKeyRow(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        auto const& t = keyLumaTable();
        std::uint32_t const groups = (width + 5) / 6;
        std::uint32_t const fullGroups = width / 6;
        constexpr std::uint32_t c = 512u;
        for (std::uint32_t g = 0; g < fullGroups; ++g)
        {
            std::uint8_t const* k = a + static_cast<std::size_t>(g) * 6;
            std::uint32_t* w = words + static_cast<std::size_t>(g) * 4;
            w[0] = c | (t[k[0]] << 10) | (c << 20);
            w[1] = t[k[1]] | (c << 10) | (t[k[2]] << 20);
            w[2] = c | (t[k[3]] << 10) | (c << 20);
            w[3] = t[k[4]] | (c << 10) | (t[k[5]] << 20);
        }
        if (fullGroups < groups)
        {
            std::uint32_t const x = fullGroups * 6;
            auto const yk = [&](std::uint32_t i) -> std::uint32_t { return i < width ? t[a[i]] : 0u; };
            auto const ck = [&](std::uint32_t i) -> std::uint32_t { return i < width ? c : 0u; };
            std::size_t const w = static_cast<std::size_t>(fullGroups) * 4;
            put(words, w + 0, ck(x), yk(x), ck(x));
            put(words, w + 1, yk(x + 1), ck(x + 2), yk(x + 2));
            put(words, w + 2, ck(x + 2), yk(x + 3), ck(x + 4));
            put(words, w + 3, yk(x + 4), ck(x + 4), yk(x + 5));
        }
        std::size_t const used = static_cast<std::size_t>(groups) * 16;
        std::memset(out + used, 0, v210RowBytes(width) - used);
    }

    void convertRows(Request const& req, PixelFn pixels, std::uint32_t firstLine, std::uint32_t endLine)
    {
        if (pixels == nullptr)
        {
            pixels = avx2Available() ? &pixelsAvx2 : &pixelsScalar;
        }
        thread_local Row row;
        row.resize(req.width);
        std::size_t const fillStride = v210RowBytes(req.width);
        std::size_t const alphaStride = alphaRowBytes(req.width);
        for (std::uint32_t line = firstLine; line < endLine; ++line)
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

    void convertFrame(Request const& req, PixelFn pixels)
    {
        convertRows(req, pixels, 0, req.height);
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
