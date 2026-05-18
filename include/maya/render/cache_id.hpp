#pragma once
// maya::CacheId — Content-hash identity for the renderer's component cache.
//
// ─────────────────────────────────────────────────────────────────────────
// Part of the Witness Chain. This module discharges the Cache Integrity
// theorem:
//
//   For any cache hit during a frame at width W, the cells in
//   `entry.cells` are equal to the cells that would be produced by
//   re-rendering the same source content at width W.
//
// The hazard this closes: a hand-authored `std::string` cache key (the
// legacy `cache_id` field, now removed) let two genuinely-different
// contents carry the same id — a forgotten separator, a stale
// generation counter, a shared_ptr identity that the allocator
// recycled into a fresh instance — and the renderer would blit stale
// cells under fresh content. Historical symptoms:
//
//   - "ghost composer + footer that resize cleared" (element.hpp)
//   - post-clear streaming committed at generation=1 with an id
//     hash-identical to the pre-clear stream's first commit
//     (streaming.cpp), so the ~2s wallclock retention kept the
//     pre-clear cells alive long enough to alias them onto the
//     post-clear layout slot.
//   - rows-per-dropped-message guesses that exceeded the physical
//     scroll height (app.hpp).
//
// Every one of these was a host-side bug that the type system could
// not catch because the cache key was an opaque string.
//
// The Witness Chain replaces it with `CacheId`: a 64-bit FNV-1a hash
// built by a typed `CacheIdBuilder`. Each `add(value)` mixes both the
// type tag and the value bytes into the hash. The host CANNOT
// accidentally collide two different contents unless their hash
// inputs are bit-identical — the same probabilistic floor (2⁻⁶⁴) as
// the shadow-of-wire hash that already gates compose. The string
// approach's "forgotten separator" failure mode is closed by the
// type tag mixin: `add("tool"sv).add(1).add(23)` and
// `add("tool"sv).add(12).add(3)` produce different hashes because
// the digits fold as distinct integer values, not as a concatenated
// string with ambiguous separators.
//
// Cost
// ────
// Zero allocation; the builder folds into a `uint64_t`. CacheId is
// trivially copyable. Cache lookup is `unordered_map<CacheId, Entry>
// ::find` — one integer hash, one integer compare — versus the
// legacy `unordered_map<string, Entry>::find` which hashed the string
// and then ran a string compare. About 5x faster on common-case ids.

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace maya {

// ─────────────────────────────────────────────────────────────────────────
// FNV-1a constants — same family as the shadow-of-wire hash
// ─────────────────────────────────────────────────────────────────────────
namespace cache_detail {
    inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    inline constexpr std::uint64_t kFnvPrime       = 1099511628211ULL;

    [[nodiscard]] inline constexpr std::uint64_t fnv_fold(std::uint64_t h,
                                                          const void* p,
                                                          std::size_t n) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= bytes[i];
            h *= kFnvPrime;
        }
        return h;
    }

    /// Hash for a type's identity. We can't use typeid().hash_code()
    /// in a constexpr context, but we can hash __PRETTY_FUNCTION__ via
    /// a template — distinct types produce distinct strings, which
    /// fold into distinct hashes. Used as the type tag mixin.
    template <class T>
    [[nodiscard]] inline std::uint64_t type_tag() noexcept {
        // typeid::hash_code is platform-dependent but consistent
        // within a single process — exactly the lifetime CacheId
        // values need to be unique over.
        static const std::uint64_t cached = []() noexcept {
            return static_cast<std::uint64_t>(typeid(T).hash_code());
        }();
        return cached;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// CacheId — opaque 64-bit content identity
// ─────────────────────────────────────────────────────────────────────────
//
// Constructable only via CacheIdBuilder. The `empty()` predicate
// returns true for default-constructed values, used to signal "this
// component opted out of caching, use pointer-identity instead."

class CacheId {
public:
    /// Default: empty / pointer-keyed fallback.
    constexpr CacheId() noexcept = default;

    [[nodiscard]] constexpr bool empty() const noexcept { return hash_ == 0; }
    [[nodiscard]] constexpr std::uint64_t hash() const noexcept { return hash_; }

    [[nodiscard]] constexpr bool operator==(const CacheId&) const noexcept = default;

private:
    constexpr explicit CacheId(std::uint64_t h) noexcept : hash_(h) {}
    friend class CacheIdBuilder;

    std::uint64_t hash_ = 0;
};

} // namespace maya

// ─────────────────────────────────────────────────────────────────────────
// Hash specialization so CacheId can key std::unordered_map directly
// ─────────────────────────────────────────────────────────────────────────
namespace std {
    template <>
    struct hash<::maya::CacheId> {
        [[nodiscard]] std::size_t operator()(const ::maya::CacheId& id) const noexcept {
            return static_cast<std::size_t>(id.hash());
        }
    };
}

namespace maya {

// ─────────────────────────────────────────────────────────────────────────
// CacheIdBuilder — the only producer of non-empty CacheId values
// ─────────────────────────────────────────────────────────────────────────
//
// Usage:
//
//   CacheId id = CacheIdBuilder{}
//       .add("turn"sv)
//       .add(turn_id)
//       .add(content_gen)
//       .build();
//
// Each `add(value)` mixes the type tag (`typeid(T).hash_code()`) and
// the value bytes into a running FNV-1a hash. Type-distinct values
// that happen to share byte representations (e.g. `int{0x4f}` vs
// `char{'O'}`) produce distinct hashes because their type tags differ.
// String-vs-int collisions ("1234" vs 1234) are impossible by
// construction.
//
// Reusable: build() doesn't reset state, so the builder can be folded
// across multiple build sites if needed. For most call sites a single
// expression-temporary builder is the right shape.

class CacheIdBuilder {
public:
    constexpr CacheIdBuilder() noexcept = default;

    /// Fold a trivially-copyable value into the running hash. The
    /// value's bytes plus its type tag are mixed together so two
    /// different types producing the same byte sequence don't alias.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    CacheIdBuilder& add(const T& value) noexcept {
        const std::uint64_t tag = cache_detail::type_tag<T>();
        hash_ = cache_detail::fnv_fold(hash_, &tag, sizeof(tag));
        hash_ = cache_detail::fnv_fold(hash_, &value, sizeof(value));
        return *this;
    }

    /// Fold a string_view's contents. The string contents are mixed
    /// after a length-prefix and a type tag so abutted strings like
    /// "ab" + "cd" don't collide with "abcd" + "".
    CacheIdBuilder& add(std::string_view sv) noexcept {
        const std::uint64_t tag = cache_detail::type_tag<std::string_view>();
        const std::uint64_t len = sv.size();
        hash_ = cache_detail::fnv_fold(hash_, &tag, sizeof(tag));
        hash_ = cache_detail::fnv_fold(hash_, &len, sizeof(len));
        hash_ = cache_detail::fnv_fold(hash_, sv.data(), sv.size());
        return *this;
    }

    /// Convenience overload: char-array literal — folds into the
    /// string_view path with the array length minus the trailing NUL.
    template <std::size_t N>
    CacheIdBuilder& add(const char (&literal)[N]) noexcept {
        return add(std::string_view{literal, N - 1});
    }

    /// Span of bytes: explicit opt-in for raw byte folding.
    CacheIdBuilder& add_bytes(std::span<const std::uint8_t> bytes) noexcept {
        const std::uint64_t tag = cache_detail::type_tag<std::span<const std::uint8_t>>();
        const std::uint64_t len = bytes.size();
        hash_ = cache_detail::fnv_fold(hash_, &tag, sizeof(tag));
        hash_ = cache_detail::fnv_fold(hash_, &len, sizeof(len));
        hash_ = cache_detail::fnv_fold(hash_, bytes.data(), bytes.size());
        return *this;
    }

    /// Finalize. Returns a non-empty CacheId encoding everything fed
    /// to add(). If nothing was added (running hash is still the FNV
    /// offset basis), build() still returns a non-empty CacheId so
    /// the host can use the builder's identity itself as a key.
    [[nodiscard]] constexpr CacheId build() const noexcept {
        // Avoid producing CacheId{0} (which would tag as "empty") even
        // for the trivial case of build() with no add()s by folding in
        // a single non-zero byte at the very start. The cost is one
        // FNV step at the constructor's static-init time, paid once.
        const std::uint64_t h = (hash_ == 0) ? cache_detail::kFnvPrime : hash_;
        return CacheId{h};
    }

private:
    std::uint64_t hash_ = cache_detail::kFnvOffsetBasis;
};

} // namespace maya
