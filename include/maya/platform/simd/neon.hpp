#pragma once
// maya::simd::Ops<Neon> - ARM NEON SIMD (128-bit, 2-4 cells/op)
//
// AArch64 baseline — always available on 64-bit ARM. Processes
// 4 cells per iteration (2x 128-bit unrolled) with branchless
// all-equal check using vminvq_u32 (single instruction on Apple
// Silicon). Falls through to scalar for tails.

#if !(defined(__aarch64__) || defined(_M_ARM64))
#error "NEON header included on non-AArch64 platform"
#endif

#include <arm_neon.h>
#if defined(__ARM_FEATURE_CRC32)
    #include <arm_acle.h>
#endif

#include <cstddef>
#include <cstdint>

#include "scalar.hpp"

namespace maya::simd {

struct Neon {};

template <>
struct Ops<Neon> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        std::size_t i = 0;

        // Unrolled: 4 cells per iteration (2x 128-bit loads)
        for (; i + 4 <= count; i += 4) {
            uint64x2_t va0 = vld1q_u64(a + i);
            uint64x2_t vb0 = vld1q_u64(b + i);
            uint64x2_t va1 = vld1q_u64(a + i + 2);
            uint64x2_t vb1 = vld1q_u64(b + i + 2);
            uint64x2_t cmp0 = vceqq_u64(va0, vb0);
            uint64x2_t cmp1 = vceqq_u64(va1, vb1);
            // Branchless all-equal: AND + horizontal min
            uint64x2_t all = vandq_u64(cmp0, cmp1);
            uint32x4_t all32 = vreinterpretq_u32_u64(all);
            if (vminvq_u32(all32) != 0) continue;
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
            uint32x4_t cmp32 = vreinterpretq_u32_u64(cmp);
            if (vminvq_u32(cmp32) != 0) continue;
            if (vgetq_lane_u64(cmp, 0) == 0) return i;
            return i + 1;
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
        // ARM has no non-temporal stores, but NEON fill is ~2x scalar
        static constexpr std::size_t kNtThreshold = 16384;
        if (count < kNtThreshold) {
            for (std::size_t i = 0; i < count; ++i) dst[i] = value;
            return;
        }

        std::size_t i = 0;
        uint64x2_t val = vdupq_n_u64(value);
        for (; i + 4 <= count; i += 4) {
            vst1q_u64(dst + i, val);
            vst1q_u64(dst + i + 2, val);
        }
        for (; i + 2 <= count; i += 2)
            vst1q_u64(dst + i, val);

        for (; i < count; ++i) dst[i] = value;
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
#if defined(__ARM_FEATURE_CRC32)
        // Hardware CRC32: ~1 cycle/cell on Apple Silicon
        uint32_t crc = 0xFFFFFFFFu;
        int i = 0;
        for (; i + 4 <= width; i += 4) {
            crc = __crc32d(crc, row[i]);
            crc = __crc32d(crc, row[i + 1]);
            crc = __crc32d(crc, row[i + 2]);
            crc = __crc32d(crc, row[i + 3]);
        }
        for (; i < width; ++i)
            crc = __crc32d(crc, row[i]);
        return static_cast<uint64_t>(crc);
#else
        return Ops<Scalar>::hash_row(row, width);
#endif
    }
};

} // namespace maya::simd
