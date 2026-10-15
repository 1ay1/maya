#pragma once
// maya::Tracked<T, Invalidator> — value wrapper that auto-runs an
// invalidator on every mutation.
//
// Why this exists
// ───────────────
// Cache-readable members of a widget have a coupling invariant: when the
// value mutates, every cache that read it must be marked stale. A naked
// `bool live_` plus a separate `bool build_dirty_` makes that coupling
// MANUAL — every assignment to live_ must also touch build_dirty_. Miss
// one, and build() returns a stale tree (the empty-trailing-box bug).
//
// Tracked<T, Inv> makes the coupling STRUCTURAL: assignment is only
// possible through .set() / operator=, and EVERY such call runs Inv{}(owner)
// before returning. The naked-write footgun becomes a compile error
// (Tracked has no `T&` conversion that yields a writable reference,
// only a `const T&` read view).
//
// Invalidator contract
// ────────────────────
// `Inv` is a stateless struct with one operation:
//     void operator()(Owner& o) const noexcept;
// Typically calls `o.mark_dirty()` or sets one or more cache-gen counters.
// The owner reference is the host widget; Tracked stores a back-pointer
// to it at construction.
//
// Cost
// ────
// Zero, by design:
//   • Empty `Inv` is [[no_unique_address]] so it adds no bytes.
//   • The owner back-pointer is one machine word, paid once per
//     tracked field. For StreamingMarkdown that's 3 fields × 8 B = 24 B
//     per instance — negligible against the multi-KB cache state.
//   • Every mutator is fully inlined: read T, write T, call Inv (which
//     becomes a direct field write). No virtual dispatch, no allocation.
//
// Usage
// ─────
//     struct StreamingMarkdown {
//         struct InvalidateBuild {
//             void operator()(StreamingMarkdown& o) const noexcept {
//                 o.build_dirty_ = true;
//             }
//         };
//         Tracked<bool, StreamingMarkdown, InvalidateBuild> live_{*this, false};
//         // …
//         void set_live(bool v) { live_ = v; }   // auto-invalidates
//         if (live_) { … }                        // implicit read via operator const T&
//     };

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace maya {

template <class T, class Owner, class Invalidator>
class Tracked {
public:
    // ── Construction ──
    // The owner is found from Tracked's OWN address, not stored: Tracked
    // keeps its byte offset inside the owner, which is the same for every
    // instance of Owner. So a defaulted copy or move of the owner (or of a
    // struct holding one, or a Model the runtime relocates) is correct by
    // construction. A stored pointer was not: it copied along with the
    // value and kept naming the moved-from owner, and the next write ran
    // the invalidator on a dead object, scribbling `build_dirty_ = true`
    // into whatever reused that memory (agent_session on clang: SIGSEGV in
    // an unrelated Element move).
    Tracked(Owner& owner, T initial) noexcept(std::is_nothrow_move_constructible_v<T>)
        : offset_(static_cast<std::ptrdiff_t>(reinterpret_cast<std::uintptr_t>(this)
                                              - reinterpret_cast<std::uintptr_t>(&owner))),
          value_(std::move(initial)) {}

    // No default ctor: a Tracked must know where its owner is.
    Tracked() = delete;

    // Copy/move carry the offset, which is the same in every Owner.
    Tracked(const Tracked&) = default;
    Tracked(Tracked&&) noexcept = default;
    Tracked& operator=(const Tracked& other) {
        if (this != &other) {
            value_ = other.value_;
            Invalidator{}(owner());
        }
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept {
        if (this != &other) {
            value_ = std::move(other.value_);
            Invalidator{}(owner());
        }
        return *this;
    }

    // ── Mutation (the only paths that exist) ──

    // Direct assign from T. The hot path.
    Tracked& operator=(T v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        value_ = std::move(v);
        Invalidator{}(owner());
        return *this;
    }

    // Explicit setter — same as operator= but reads better at call sites
    // that want to be obviously-a-write ("live_.set(false)" vs "live_ = false").
    void set(T v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        value_ = std::move(v);
        Invalidator{}(owner());
    }

    // ── Read ──

    // Implicit conversion to const T& — every read site stays
    // syntactically `if (live_)`, `source_version_ + 1`, etc., with no
    // refactor at use sites.
    operator const T&() const noexcept { return value_; }

    // Explicit accessor for cases where the implicit conversion is
    // ambiguous (e.g. inside templates, or when calling member functions
    // on the value).
    [[nodiscard]] const T& get() const noexcept { return value_; }

    // ── Owner rebinding ──
    // Nothing to do: the owner is located by offset, so it follows every
    // copy and move on its own. Kept so existing callers still compile.
    void rebind(Owner&) noexcept {}

    // ── Mutation helpers for arithmetic types ──
    // Pre-increment: bump and invalidate. Returns the new value by
    // value (NOT a reference into Tracked's storage — that would let
    // the caller write through the read and skip the invalidator).
    template <class U = T,
              class = std::enable_if_t<std::is_arithmetic_v<U>>>
    T operator++() noexcept {
        ++value_;
        Invalidator{}(owner());
        return value_;
    }

    // Post-increment.
    template <class U = T,
              class = std::enable_if_t<std::is_arithmetic_v<U>>>
    T operator++(int) noexcept {
        T old = value_;
        ++value_;
        Invalidator{}(owner());
        return old;
    }

private:
    [[nodiscard]] Owner& owner() const noexcept {
        return *reinterpret_cast<Owner*>(reinterpret_cast<std::uintptr_t>(this) - offset_);
    }

    std::ptrdiff_t offset_;   // this - &owner; identical in every Owner
    [[no_unique_address]] Invalidator inv_{};  // EBO: 0 B for empty stateless invalidator
    T value_;
};

} // namespace maya
