// SPDX-License-Identifier: MIT
// AVX2 pass 1 (SPEC §5.2): the same integer arithmetic as pixelsScalar, eight pixels at a time.
// Only this function is compiled for AVX2 (target attribute); the binary runs on x86-64-v2.
#include "convert/convert.hpp"

#include <immintrin.h>

#include <cstring>

namespace mbs::convert
{
    bool avx2Available()
    {
        static bool const available = __builtin_cpu_supports("avx2") != 0;
        return available;
    }

    __attribute__((target("avx2"))) void pixelsAvx2(std::uint8_t const* bgra, std::uint32_t count, bool straight, std::int32_t* y, std::int32_t* cb,
        std::int32_t* cr, std::uint8_t* a)
    {
        auto const& k = coefficients();
        auto const* table = reinterpret_cast<int const*>(unpremultiplyTable());
        __m256i const mask = _mm256_set1_epi32(0xFF);
        __m256i const max8 = _mm256_set1_epi32(255);
        __m256i const half16 = _mm256_set1_epi32(32768);
        __m256i const half14 = _mm256_set1_epi32(1 << 13);
        __m256i const y64 = _mm256_set1_epi32(64);
        __m256i const lo10 = _mm256_set1_epi32(4);
        __m256i const hi10 = _mm256_set1_epi32(1019);
        __m256i const kyR = _mm256_set1_epi32(k.yR), kyG = _mm256_set1_epi32(k.yG), kyB = _mm256_set1_epi32(k.yB);
        __m256i const kbR = _mm256_set1_epi32(k.cbR), kbG = _mm256_set1_epi32(k.cbG), kbB = _mm256_set1_epi32(k.cbB);
        __m256i const krR = _mm256_set1_epi32(k.crR), krG = _mm256_set1_epi32(k.crG), krB = _mm256_set1_epi32(k.crB);

        std::uint32_t i = 0;
        for (; i + 8 <= count; i += 8)
        {
            __m256i const px = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(bgra + 4 * i));
            __m256i b = _mm256_and_si256(px, mask);
            __m256i g = _mm256_and_si256(_mm256_srli_epi32(px, 8), mask);
            __m256i r = _mm256_and_si256(_mm256_srli_epi32(px, 16), mask);
            __m256i const al = _mm256_srli_epi32(px, 24);
            // Opaque pixels are their own straight colour (table[255] = 65536 is the identity) and
            // transparent ones become 0 (table[0] = 0); only mixed alpha needs the table gather.
            int const opaque = _mm256_movemask_epi8(_mm256_cmpeq_epi32(al, max8));
            if (straight && opaque != -1)
            {
                if (_mm256_testz_si256(al, al) != 0)
                {
                    b = g = r = _mm256_setzero_si256();
                }
                else
                {
                    __m256i const t = _mm256_i32gather_epi32(table, al, 4);
                    b = _mm256_min_epu32(max8, _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(b, t), half16), 16));
                    g = _mm256_min_epu32(max8, _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(g, t), half16), 16));
                    r = _mm256_min_epu32(max8, _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(r, t), half16), 16));
                }
            }
            __m256i ys = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(kyR, r), _mm256_mullo_epi32(kyG, g)), _mm256_mullo_epi32(kyB, b));
            ys = _mm256_add_epi32(y64, _mm256_srai_epi32(_mm256_add_epi32(ys, half14), 14));
            ys = _mm256_min_epi32(hi10, _mm256_max_epi32(lo10, ys));
            __m256i const cbs = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(kbR, r), _mm256_mullo_epi32(kbG, g)), _mm256_mullo_epi32(kbB, b));
            __m256i const crs = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(krR, r), _mm256_mullo_epi32(krG, g)), _mm256_mullo_epi32(krB, b));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(y + i), ys);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(cb + i), cbs);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(cr + i), crs);
            // Alpha 32 → 16 → 8 bit: each 128-bit lane ends with its four alphas in its low bytes.
            __m256i const a16 = _mm256_packus_epi32(al, al);
            __m256i const a8 = _mm256_packus_epi16(a16, a16);
            auto const lo = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm256_castsi256_si128(a8)));
            auto const hi = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm256_extracti128_si256(a8, 1)));
            std::memcpy(a + i, &lo, 4);
            std::memcpy(a + i + 4, &hi, 4);
        }
        if (i < count)
        {
            pixelsScalar(bgra + 4 * i, count - i, straight, y + i, cb + i, cr + i, a + i);
        }
    }
}

namespace mbs::convert
{
    // out[x] = clamp(512 + ((c[x−1] + 2c[x] + c[x+1] + 2^15) >> 16), 4, 1019) for first ≤ x < end;
    // the caller keeps x−1 and x+1 inside the row. The same integers as the scalar filter.
    __attribute__((target("avx2"))) void chromaRowAvx2(std::int32_t const* c, std::uint32_t first, std::uint32_t end, std::uint32_t* out)
    {
        __m256i const half = _mm256_set1_epi32(1 << 15);
        __m256i const mid = _mm256_set1_epi32(512);
        __m256i const lo = _mm256_set1_epi32(4);
        __m256i const hi = _mm256_set1_epi32(1019);
        std::uint32_t x = first;
        for (; x + 8 <= end; x += 8)
        {
            __m256i const left = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(c + x - 1));
            __m256i const centre = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(c + x));
            __m256i const right = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(c + x + 1));
            __m256i sum = _mm256_add_epi32(_mm256_add_epi32(left, right), _mm256_add_epi32(centre, centre));
            sum = _mm256_add_epi32(mid, _mm256_srai_epi32(_mm256_add_epi32(sum, half), 16));
            sum = _mm256_min_epi32(hi, _mm256_max_epi32(lo, sum));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + x), sum);
        }
        for (; x < end; ++x)
        {
            std::int32_t const v = 512 + ((c[x - 1] + 2 * c[x] + c[x + 1] + (1 << 15)) >> 16);
            out[x] = static_cast<std::uint32_t>(v < 4 ? 4 : (v > 1019 ? 1019 : v));
        }
    }
}
