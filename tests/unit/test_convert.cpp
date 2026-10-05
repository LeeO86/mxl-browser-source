// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

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
