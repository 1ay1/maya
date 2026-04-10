#pragma once
// maya::simd::Ops<Avx2> - AVX2 SIMD (256-bit, 4 cells/op)
//
// Processes 4 packed cells (256 bits) per comparison.
// Falls through to scalar for remaining elements.

#if !defined(__AVX2__)
#error "AVX2 header included without __AVX2__ defined"
#endif

#include <bit>
#include <immintrin.h>
#include <cstddef>
#include <cstdint>

#include "../detect.hpp"
#include "sse2.hpp"

namespace maya::simd {

struct Avx2 {};

template <>
struct Ops<Avx2> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        std::size_t i = 0;

        // AVX2 with 64-bit lane compare: 4 cells per iteration, producing
        // a 4-bit mask directly via movemask_pd — avoids the /8 divide of
        // the byte-level approach.
        for (; i + 4 <= count; i += 4) {
            __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            __m256i vb  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            __m256i cmp = _mm256_cmpeq_epi64(va, vb);
            unsigned mask = static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)));
            if (mask != 0xFu) {
                return i + static_cast<std::size_t>(
                    std::countr_zero(static_cast<unsigned>(~mask & 0xFu)));
            }
        }

        // Scalar tail
        for (; i < count; ++i) {
            if (a[i] != b[i]) return i;
        }
        return count;
    }

    [[nodiscard]] static std::size_t skip_equal(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t start,
        std::size_t end) noexcept
    {
        std::size_t i = start;

        for (; i + 4 <= end; i += 4) {
            __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            __m256i vb  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            __m256i cmp = _mm256_cmpeq_epi64(va, vb);
            unsigned mask = static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)));
            if (mask != 0xFu) {
                return i + static_cast<std::size_t>(
                    std::countr_zero(static_cast<unsigned>(~mask & 0xFu)));
            }
        }

        for (; i < end; ++i) {
            if (a[i] != b[i]) return i;
        }
        return end;
    }

    static void streaming_fill(
        uint64_t* MAYA_RESTRICT dst,
        std::size_t count,
        uint64_t value) noexcept
    {
        static constexpr std::size_t kNtThreshold = 16384;
        if (count < kNtThreshold) {
            for (std::size_t i = 0; i < count; ++i) dst[i] = value;
            return;
        }

        std::size_t i = 0;
        __m256i val = _mm256_set1_epi64x(static_cast<long long>(value));
        for (; i + 4 <= count; i += 4)
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i), val);

        for (; i < count; ++i) dst[i] = value;
        _mm_sfence();
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
        // AVX2 implies SSE4.2 — use hardware CRC32
        return Ops<Sse2>::hash_row(row, width);
    }
};

} // namespace maya::simd
