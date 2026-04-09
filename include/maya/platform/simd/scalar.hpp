#pragma once
// maya::simd::Ops<Scalar> - Portable scalar fallback
//
// Always compiles on every platform. Used as the fallback when no
// SIMD instruction set is available, and for scalar tails after
// SIMD main loops.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace maya::simd {

struct Scalar {};

template <typename Isa> struct Ops;

template <>
struct Ops<Scalar> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        for (std::size_t i = 0; i < count; ++i) {
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
        for (std::size_t i = start; i < end; ++i) {
            if (a[i] != b[i]) return i;
        }
        return end;
    }

    static void streaming_fill(
        uint64_t* MAYA_RESTRICT dst,
        std::size_t count,
        uint64_t value) noexcept
    {
        for (std::size_t i = 0; i < count; ++i) dst[i] = value;
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
        // FNV-1a
        uint64_t h = 14695981039346656037ULL;
        for (int i = 0; i < width; ++i) {
            h ^= row[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
};

} // namespace maya::simd
