// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "convert/convert.hpp"

namespace cv = mbs::convert;

namespace
{
    // Random BGRA with every edge the converters treat specially: alpha 0 and 255, colour
    // above alpha (not premultiplied: must clamp, not wrap), black and white.
    std::vector<std::uint8_t> raster(std::uint32_t width, std::uint32_t height, std::uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            switch (rng() % 6)
            {
            case 0: px[i] = px[i + 1] = px[i + 2] = px[i + 3] = 0; break;
            case 1: px[i] = px[i + 1] = px[i + 2] = px[i + 3] = 255; break;
            case 2: px[i] = static_cast<std::uint8_t>(rng()); px[i + 1] = static_cast<std::uint8_t>(rng()); px[i + 2] = static_cast<std::uint8_t>(rng()); px[i + 3] = 255; break;
            default:
            {
                auto const a = static_cast<std::uint8_t>(rng());
                for (int c = 0; c < 3; ++c)
                {
                    px[i + c] = static_cast<std::uint8_t>(rng() % 5 == 0 ? rng() : (a == 0 ? 0 : rng() % (a + 1u)));
                }
                px[i + 3] = a;
            }
            }
        }
        return px;
    }

    struct Planes
    {
        std::vector<std::uint8_t> fill, alpha, key;
    };

    Planes convert(std::vector<std::uint8_t> const& px, std::uint32_t width, std::uint32_t height, bool straight, cv::PixelFn fn)
    {
        Planes out;
        out.fill.assign(static_cast<std::size_t>(cv::v210RowBytes(width)) * height, 0xAA);
        out.alpha.assign(static_cast<std::size_t>(cv::alphaRowBytes(width)) * height, 0xAA);
        out.key.assign(static_cast<std::size_t>(cv::v210RowBytes(width)) * height, 0xAA);
        cv::Request r;
        r.bgra = px.data();
        r.bgraStride = static_cast<std::size_t>(width) * 4;
        r.width = width;
        r.height = height;
        r.straight = straight;
        r.fill = out.fill.data();
        r.alpha = out.alpha.data();
        r.key = out.key.data();
        cv::convertFrame(r, fn);
        return out;
    }

    std::uint32_t word(std::vector<std::uint8_t> const& plane, std::size_t index)
    {
        std::uint32_t w = 0;
        std::memcpy(&w, plane.data() + index * 4, 4);
        return w;
    }
}

TEST_CASE("v210 and v210a line lengths")
{
    CHECK(cv::v210RowBytes(1920) == 5120);
    CHECK(cv::v210RowBytes(1280) == 3456);
    CHECK(cv::v210RowBytes(6) == 128);
    CHECK(cv::alphaRowBytes(1920) == 2560);
    CHECK(cv::alphaRowBytes(4) == 8);
}

TEST_CASE("known colours: opaque white and black, transparent")
{
    std::vector<std::uint8_t> white(6 * 4, 255);
    auto const w = convert(white, 6, 1, false, cv::pixelsScalar);
    // Word 0 is Cb0 | Y0 << 10 | Cr0 << 20.
    CHECK((word(w.fill, 0) & 0x3FF) == 512);
    CHECK(((word(w.fill, 0) >> 10) & 0x3FF) == 940);
    CHECK(((word(w.fill, 0) >> 20) & 0x3FF) == 512);
    CHECK((word(w.alpha, 0) & 0x3FF) == 1023);
    CHECK(((word(w.key, 0) >> 10) & 0x3FF) == 940);

    std::vector<std::uint8_t> clear(6 * 4, 0);
    auto const c = convert(clear, 6, 1, true, cv::pixelsScalar);
    CHECK(((word(c.fill, 0) >> 10) & 0x3FF) == 64);
    CHECK((word(c.fill, 0) & 0x3FF) == 512);
    CHECK((word(c.alpha, 0) & 0x3FF) == 0);
    CHECK(((word(c.key, 0) >> 10) & 0x3FF) == 64);
    CHECK((word(c.key, 0) & 0x3FF) == 512);
}

TEST_CASE("AVX2 pass 1 gives the scalar integers")
{
    if (!cv::avx2Available())
    {
        MESSAGE("no AVX2 on this CPU: skipped");
        return;
    }
    for (std::uint32_t const width : {1u, 6u, 7u, 8u, 9u, 15u, 16u, 17u, 47u, 48u, 49u, 720u, 1280u, 1920u, 3840u})
    {
        for (bool const straight : {false, true})
        {
            CAPTURE(width);
            CAPTURE(straight);
            auto const px = raster(width, 1, width * 7 + (straight ? 1 : 0));
            std::vector<std::int32_t> y1(width), cb1(width), cr1(width), y2(width), cb2(width), cr2(width);
            std::vector<std::uint8_t> a1(width), a2(width);
            cv::pixelsScalar(px.data(), width, straight, y1.data(), cb1.data(), cr1.data(), a1.data());
            cv::pixelsAvx2(px.data(), width, straight, y2.data(), cb2.data(), cr2.data(), a2.data());
            CHECK(y1 == y2);
            CHECK(cb1 == cb2);
            CHECK(cr1 == cr2);
            CHECK(a1 == a2);
        }
    }
}

TEST_CASE("whole frames are byte-identical with either pass 1")
{
    if (!cv::avx2Available())
    {
        MESSAGE("no AVX2 on this CPU: skipped");
        return;
    }
    for (std::uint32_t const width : {6u, 50u, 1920u})
    {
        for (bool const straight : {false, true})
        {
            CAPTURE(width);
            CAPTURE(straight);
            auto const px = raster(width, 4, width + 100);
            auto const a = convert(px, width, 4, straight, cv::pixelsScalar);
            auto const b = convert(px, width, 4, straight, cv::pixelsAvx2);
            CHECK(a.fill == b.fill);
            CHECK(a.alpha == b.alpha);
            CHECK(a.key == b.key);
        }
    }
}

namespace
{
    // The pass-2 packers before the table and interior-loop versions, as the reference.
    std::int32_t refChroma(std::int32_t l, std::int32_t c, std::int32_t r)
    {
        return std::clamp(512 + ((l + 2 * c + r + (1 << 15)) >> 16), 4, 1019);
    }

    void refPut(std::uint32_t* w, std::size_t i, std::uint32_t a, std::uint32_t b, std::uint32_t c)
    {
        w[i] = (a & 0x3FFu) | ((b & 0x3FFu) << 10) | ((c & 0x3FFu) << 20);
    }

    void refPackFill(cv::Row const& row, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        std::uint32_t const groups = (width + 5) / 6;
        auto const last = static_cast<std::int64_t>(width) - 1;
        auto const at = [&](std::vector<std::int32_t> const& v, std::int64_t x) { return v[static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, last))]; };
        auto const yAt = [&](std::uint32_t x) -> std::uint32_t { return x < width ? static_cast<std::uint32_t>(row.y[x]) : 0u; };
        auto const cAt = [&](std::vector<std::int32_t> const& v, std::uint32_t x) -> std::uint32_t {
            return x >= width ? 0u : static_cast<std::uint32_t>(refChroma(at(v, static_cast<std::int64_t>(x) - 1), v[x], at(v, static_cast<std::int64_t>(x) + 1)));
        };
        for (std::uint32_t g = 0; g < groups; ++g)
        {
            std::uint32_t const x = g * 6;
            std::size_t const w = static_cast<std::size_t>(g) * 4;
            refPut(words, w + 0, cAt(row.cb, x), yAt(x), cAt(row.cr, x));
            refPut(words, w + 1, yAt(x + 1), cAt(row.cb, x + 2), yAt(x + 2));
            refPut(words, w + 2, cAt(row.cr, x + 2), yAt(x + 3), cAt(row.cb, x + 4));
            refPut(words, w + 3, yAt(x + 4), cAt(row.cr, x + 4), yAt(x + 5));
        }
        std::size_t const used = static_cast<std::size_t>(groups) * 16;
        std::memset(out + used, 0, cv::v210RowBytes(width) - used);
    }

    void refPackAlpha(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        auto const a10 = [&](std::uint32_t x) -> std::uint32_t { return x < width ? (static_cast<std::uint32_t>(a[x]) * 1023u + 127u) / 255u : 0u; };
        for (std::uint32_t i = 0; i < (width + 2) / 3; ++i)
        {
            refPut(words, i, a10(3 * i), a10(3 * i + 1), a10(3 * i + 2));
        }
    }

    void refPackKey(std::uint8_t const* a, std::uint32_t width, std::uint8_t* out)
    {
        auto* words = reinterpret_cast<std::uint32_t*>(out);
        auto const yk = [&](std::uint32_t x) -> std::uint32_t { return x < width ? 64u + (static_cast<std::uint32_t>(a[x]) * 876u + 127u) / 255u : 0u; };
        auto const ck = [&](std::uint32_t x) -> std::uint32_t { return x < width ? 512u : 0u; };
        for (std::uint32_t g = 0; g < (width + 5) / 6; ++g)
        {
            std::uint32_t const x = g * 6;
            std::size_t const w = static_cast<std::size_t>(g) * 4;
            refPut(words, w + 0, ck(x), yk(x), ck(x));
            refPut(words, w + 1, yk(x + 1), ck(x + 2), yk(x + 2));
            refPut(words, w + 2, ck(x + 2), yk(x + 3), ck(x + 4));
            refPut(words, w + 3, yk(x + 4), ck(x + 4), yk(x + 5));
        }
        std::size_t const used = static_cast<std::size_t>((width + 5) / 6) * 16;
        std::memset(out + used, 0, cv::v210RowBytes(width) - used);
    }
}

TEST_CASE("the pass-2 packers give the reference bytes at every width")
{
    std::mt19937 rng(17);
    for (std::uint32_t width : {1u, 2u, 3u, 5u, 6u, 7u, 11u, 12u, 13u, 47u, 48u, 49u, 719u, 720u, 1280u, 1281u, 1919u, 1920u, 3840u})
    {
        cv::Row row;
        row.resize(width);
        std::vector<std::uint8_t> a(width + 8);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            row.y[x] = static_cast<std::int32_t>(4 + rng() % 1016);
            row.cb[x] = static_cast<std::int32_t>(rng() % 2000001) - 1000000; // around ±0.5·896·2^14
            row.cr[x] = static_cast<std::int32_t>(rng() % 2000001) - 1000000;
            a[x] = static_cast<std::uint8_t>(rng());
        }
        std::vector<std::uint8_t> expected(cv::v210RowBytes(width) + 16, 0xAA), actual(expected);
        refPackFill(row, width, expected.data());
        cv::packFillRow(row, width, actual.data());
        CHECK(expected == actual);
        std::vector<std::uint8_t> ea(cv::alphaRowBytes(width) + 16, 0xAA), aa(ea);
        refPackAlpha(a.data(), width, ea.data());
        cv::packAlphaRow(a.data(), width, aa.data());
        CHECK(ea == aa);
        std::vector<std::uint8_t> ek(cv::v210RowBytes(width) + 16, 0xAA), ak(ek);
        refPackKey(a.data(), width, ek.data());
        cv::packKeyRow(a.data(), width, ak.data());
        CHECK(ek == ak);
    }
}

TEST_CASE("bands of rows give the same frame as one pass")
{
    std::uint32_t const width = 1281;
    std::uint32_t const height = 37;
    auto const px = raster(width, height, 99);
    auto const whole = convert(px, width, height, true, nullptr);
    Planes banded;
    banded.fill.assign(whole.fill.size(), 0xAA);
    banded.alpha.assign(whole.alpha.size(), 0xAA);
    banded.key.assign(whole.key.size(), 0xAA);
    cv::Request r;
    r.bgra = px.data();
    r.bgraStride = static_cast<std::size_t>(width) * 4;
    r.width = width;
    r.height = height;
    r.straight = true;
    r.fill = banded.fill.data();
    r.alpha = banded.alpha.data();
    r.key = banded.key.data();
    cv::convertRows(r, nullptr, 20, height);
    cv::convertRows(r, nullptr, 0, 7);
    cv::convertRows(r, nullptr, 7, 20);
    CHECK(banded.fill == whole.fill);
    CHECK(banded.alpha == whole.alpha);
    CHECK(banded.key == whole.key);
}

TEST_CASE("AVX2 straight fill: runs of opaque and transparent pixels give the scalar bytes")
{
    if (!cv::avx2Available())
    {
        return;
    }
    // A graphics page: long opaque runs (any colour), fully transparent runs (colour may be
    // non-zero, must become 0), and mixed-alpha runs, at odd offsets against the 8-pixel steps.
    std::uint32_t const width = 1923;
    std::uint32_t const height = 4;
    std::mt19937 rng(5);
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t p = 0; p < px.size() / 4; ++p)
    {
        auto const kind = (p / 37) % 3;
        px[4 * p] = static_cast<std::uint8_t>(rng());
        px[4 * p + 1] = static_cast<std::uint8_t>(rng());
        px[4 * p + 2] = static_cast<std::uint8_t>(rng());
        px[4 * p + 3] = kind == 0 ? 255 : (kind == 1 ? 0 : static_cast<std::uint8_t>(rng()));
    }
    auto const scalar = convert(px, width, height, true, &cv::pixelsScalar);
    auto const avx2 = convert(px, width, height, true, &cv::pixelsAvx2);
    CHECK(scalar.fill == avx2.fill);
    CHECK(scalar.alpha == avx2.alpha);
    CHECK(scalar.key == avx2.key);
}
