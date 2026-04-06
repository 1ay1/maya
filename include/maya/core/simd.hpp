#pragma once
// maya::core::simd - SIMD-accelerated bulk operations for terminal cells
//
// Hardware-accelerated comparison of packed 64-bit cell arrays. Uses
// AVX-512F (8 cells/cycle), AVX2 (4 cells/cycle), SSE2 (2 cells/cycle,
// x86-64 baseline), or NEON (2 cells/cycle, ARM64). Scalar fallback for
// everything else.
//
// The diff algorithm calls find_first_diff() to skip unchanged regions
// in O(N/8) instead of O(N). For typical incremental updates the damage
// region is small, but SIMD still helps after terminal resize or when
// scrolling.
//
// Also provides streaming_fill() for non-temporal (write-combining) bulk
// fill — used by Canvas::clear() to avoid polluting L1/L2 cache with data
// that on_paint() will overwrite immediately.

#include <cstddef>
#include <cstdint>
#include <cstring>

// -- ISA detection -----------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #if defined(__AVX512F__)
        #define MAYA_SIMD_AVX512 1
    #endif
    #if defined(__AVX2__)
        #define MAYA_SIMD_AVX2 1
    #endif
    // SSE2 is baseline for all x86-64 CPUs.
    #define MAYA_SIMD_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define MAYA_SIMD_NEON 1
#endif

namespace maya::simd {

// ============================================================================
// find_first_diff - index of first differing cell in two arrays
// ============================================================================
// Returns `count` if all cells are identical.

[[nodiscard]] inline std::size_t find_first_diff(
    const uint64_t* __restrict__ a,
    const uint64_t* __restrict__ b,
    std::size_t count) noexcept
{
    std::size_t i = 0;

#if defined(MAYA_SIMD_AVX512)
    // AVX-512F: 8 cells (512 bits = 64 bytes) per iteration.
    // Compare 64-bit lanes directly — no byte-mask + divide-by-8 needed.
    for (; i + 8 <= count; i += 8) {
        __m512i va   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(a + i));
        __m512i vb   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(b + i));
        __mmask8 neq = _mm512_cmpneq_epi64_mask(va, vb);
        if (neq != 0) {
            return i + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(neq)));
        }
    }
#endif

#if defined(MAYA_SIMD_AVX2)
    // AVX2: 4 cells (256 bits = 32 bytes) per iteration.
    for (; i + 4 <= count; i += 4) {
        __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i cmp = _mm256_cmpeq_epi8(va, vb);  // 0xFF=equal, 0x00=different
        unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(cmp));
        if (mask != 0xFFFFFFFFu) {
            // At least one byte differs. Find which cell (8 bytes each).
            int diff_byte = __builtin_ctz(~mask);
            return i + static_cast<std::size_t>(diff_byte / 8);
        }
    }
#elif defined(MAYA_SIMD_SSE2)
    // SSE2: 2 cells (128 bits = 16 bytes) per iteration.
    for (; i + 2 <= count; i += 2) {
        __m128i va  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i vb  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i cmp = _mm_cmpeq_epi8(va, vb);
        int mask    = _mm_movemask_epi8(cmp);
        if (mask != 0xFFFF) {
            int diff_byte = __builtin_ctz(static_cast<unsigned>(~mask & 0xFFFF));
            return i + static_cast<std::size_t>(diff_byte / 8);
        }
    }
#elif defined(MAYA_SIMD_NEON)
    // NEON: 4 cells (2x 128 bits) per iteration — unrolled for throughput.
    for (; i + 4 <= count; i += 4) {
        uint64x2_t va0 = vld1q_u64(a + i);
        uint64x2_t vb0 = vld1q_u64(b + i);
        uint64x2_t va1 = vld1q_u64(a + i + 2);
        uint64x2_t vb1 = vld1q_u64(b + i + 2);
        uint64x2_t cmp0 = vceqq_u64(va0, vb0);
        uint64x2_t cmp1 = vceqq_u64(va1, vb1);
        // Fast path: all 4 cells equal (common case for unchanged regions)
        uint64x2_t all = vandq_u64(cmp0, cmp1);
        if (vgetq_lane_u64(all, 0) != 0 && vgetq_lane_u64(all, 1) != 0)
            continue;
        // Slow path: find which cell differs
        if (vgetq_lane_u64(cmp0, 0) == 0) return i;
        if (vgetq_lane_u64(cmp0, 1) == 0) return i + 1;
        if (vgetq_lane_u64(cmp1, 0) == 0) return i + 2;
        return i + 3;
    }
    // Handle remaining 2-cell block
    for (; i + 2 <= count; i += 2) {
        uint64x2_t va  = vld1q_u64(a + i);
        uint64x2_t vb  = vld1q_u64(b + i);
        uint64x2_t cmp = vceqq_u64(va, vb);
        if (vgetq_lane_u64(cmp, 0) == 0) return i;
        if (vgetq_lane_u64(cmp, 1) == 0) return i + 1;
    }
#endif

    // Scalar tail.
    for (; i < count; ++i) {
        if (a[i] != b[i]) return i;
    }
    return count;
}

// ============================================================================
// skip_equal - find next differing cell starting from `start`
// ============================================================================
// Returns the index of the first differing cell (>= start), or `end` if all
// cells in [start, end) are identical. Used by the diff inner loop to jump
// over unchanged runs within a row.

[[nodiscard]] inline std::size_t skip_equal(
    const uint64_t* __restrict__ a,
    const uint64_t* __restrict__ b,
    std::size_t start,
    std::size_t end) noexcept
{
    std::size_t i = start;

#if defined(MAYA_SIMD_AVX512)
    for (; i + 8 <= end; i += 8) {
        __m512i va   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(a + i));
        __m512i vb   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(b + i));
        __mmask8 neq = _mm512_cmpneq_epi64_mask(va, vb);
        if (neq != 0) {
            return i + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(neq)));
        }
    }
#endif

#if defined(MAYA_SIMD_AVX2)
    for (; i + 4 <= end; i += 4) {
        __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i cmp = _mm256_cmpeq_epi8(va, vb);
        unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(cmp));
        if (mask != 0xFFFFFFFFu) {
            int diff_byte = __builtin_ctz(~mask);
            return i + static_cast<std::size_t>(diff_byte / 8);
        }
    }
#elif defined(MAYA_SIMD_SSE2)
    for (; i + 2 <= end; i += 2) {
        __m128i va  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i vb  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i cmp = _mm_cmpeq_epi8(va, vb);
        int mask    = _mm_movemask_epi8(cmp);
        if (mask != 0xFFFF) {
            int diff_byte = __builtin_ctz(static_cast<unsigned>(~mask & 0xFFFF));
            return i + static_cast<std::size_t>(diff_byte / 8);
        }
    }
#elif defined(MAYA_SIMD_NEON)
    for (; i + 4 <= end; i += 4) {
        uint64x2_t va0 = vld1q_u64(a + i);
        uint64x2_t vb0 = vld1q_u64(b + i);
        uint64x2_t va1 = vld1q_u64(a + i + 2);
        uint64x2_t vb1 = vld1q_u64(b + i + 2);
        uint64x2_t cmp0 = vceqq_u64(va0, vb0);
        uint64x2_t cmp1 = vceqq_u64(va1, vb1);
        uint64x2_t all = vandq_u64(cmp0, cmp1);
        if (vgetq_lane_u64(all, 0) != 0 && vgetq_lane_u64(all, 1) != 0)
            continue;
        if (vgetq_lane_u64(cmp0, 0) == 0) return i;
        if (vgetq_lane_u64(cmp0, 1) == 0) return i + 1;
        if (vgetq_lane_u64(cmp1, 0) == 0) return i + 2;
        return i + 3;
    }
    for (; i + 2 <= end; i += 2) {
        uint64x2_t va  = vld1q_u64(a + i);
        uint64x2_t vb  = vld1q_u64(b + i);
        uint64x2_t cmp = vceqq_u64(va, vb);
        if (vgetq_lane_u64(cmp, 0) == 0) return i;
        if (vgetq_lane_u64(cmp, 1) == 0) return i + 1;
    }
#endif

    for (; i < end; ++i) {
        if (a[i] != b[i]) return i;
    }
    return end;
}

// ============================================================================
// bulk_eq - check if entire range is identical
// ============================================================================
// Fast early-out for rows where nothing changed.

[[nodiscard]] inline bool bulk_eq(
    const uint64_t* __restrict__ a,
    const uint64_t* __restrict__ b,
    std::size_t count) noexcept
{
    return find_first_diff(a, b, count) == count;
}

// ============================================================================
// streaming_fill - non-temporal bulk fill (bypasses cache)
// ============================================================================
// Writes `value` to `count` uint64_t slots using streaming (non-temporal)
// stores. Data goes directly to memory through the write-combine buffer,
// avoiding L1/L2 cache pollution. Used by Canvas::clear() so that the cache
// stays warm for on_paint() writes and diff reads.
//
// Falls back to regular std::fill for small counts where cache pollution
// is not a concern and for architectures without NT store support.

inline void streaming_fill(uint64_t* __restrict__ dst, std::size_t count, uint64_t value) noexcept {
    // For buffers that fit in L1 cache, regular fill is faster (data stays
    // warm for the immediately-following paint + diff reads). NT stores
    // bypass cache and cause cold misses. 16384 cells = 128KB covers
    // typical full-screen terminals (e.g. 200x50 = 10K cells = 80KB).
    static constexpr std::size_t kNtThreshold = 16384; // ~128KB
    if (count < kNtThreshold) {
        std::memset(dst, 0, 0); // compiler barrier
        for (std::size_t i = 0; i < count; ++i) dst[i] = value;
        return;
    }

    std::size_t i = 0;

#if defined(MAYA_SIMD_AVX512)
    {
        __m512i val = _mm512_set1_epi64(static_cast<long long>(value));
        for (; i + 8 <= count; i += 8)
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst + i), val);
    }
#elif defined(MAYA_SIMD_AVX2)
    {
        __m256i val = _mm256_set1_epi64x(static_cast<long long>(value));
        for (; i + 4 <= count; i += 4)
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i), val);
    }
#elif defined(MAYA_SIMD_SSE2)
    {
        __m128i val = _mm_set1_epi64x(static_cast<long long>(value));
        for (; i + 2 <= count; i += 2)
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), val);
    }
#elif defined(MAYA_SIMD_NEON)
    // ARM64 has no non-temporal stores, but NEON vectorized fill is still
    // ~2x faster than scalar (128-bit writes vs 64-bit). Use vst1q_u64
    // for 2 cells per store, unrolled 2x for 4 cells per iteration.
    {
        uint64x2_t val = vdupq_n_u64(value);
        for (; i + 4 <= count; i += 4) {
            vst1q_u64(dst + i, val);
            vst1q_u64(dst + i + 2, val);
        }
        for (; i + 2 <= count; i += 2)
            vst1q_u64(dst + i, val);
    }
#endif

    // Scalar tail
    for (; i < count; ++i) dst[i] = value;

#if defined(MAYA_SIMD_SSE2) || defined(MAYA_SIMD_AVX2) || defined(MAYA_SIMD_AVX512)
    _mm_sfence(); // ensure NT stores are globally visible
#endif
}

} // namespace maya::simd
