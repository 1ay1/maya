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

#include <type_traits>
#include <utility>

namespace maya {

template <class T, class Owner, class Invalidator>
class Tracked {
public:
    // ── Construction ──
    // Bind to the owner at construction. The owner reference is stored as
    // a pointer (not a reference member) so Tracked stays move-assignable
    // when the owner itself is moved — the new owner's move-constructor
    // is responsible for rebinding via `rebind()` if needed.
    Tracked(Owner& owner, T initial) noexcept(std::is_nothrow_move_constructible_v<T>)
        : owner_(&owner), value_(std::move(initial)) {}

    // No default ctor: a Tracked without an owner is a footgun (writes
    // would crash on null deref). Owners must explicitly construct it.
    Tracked() = delete;

    // Copyable/movable; on move the owner pointer copies too. If the
    // OWNER itself is being moved, it must call `rebind(*this)` on each
    // Tracked member from its own move-ctor — Tracked can't detect that.
    Tracked(const Tracked&) = default;
    Tracked(Tracked&&) noexcept = default;
    Tracked& operator=(const Tracked& other) {
        if (this != &other) {
            value_ = other.value_;
            Invalidator{}(*owner_);
        }
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept {
        if (this != &other) {
            value_ = std::move(other.value_);
            Invalidator{}(*owner_);
        }
        return *this;
    }

    // ── Mutation (the only paths that exist) ──

    // Direct assign from T. The hot path.
    Tracked& operator=(T v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        value_ = std::move(v);
        Invalidator{}(*owner_);
        return *this;
    }

    // Explicit setter — same as operator= but reads better at call sites
    // that want to be obviously-a-write ("live_.set(false)" vs "live_ = false").
    void set(T v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        value_ = std::move(v);
        Invalidator{}(*owner_);
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
    // Called by the OWNER's move-constructor/move-assignment. Necessary
    // because Tracked stores a back-pointer; if the owner relocates and
    // doesn't rebind, the back-pointer still points at the moved-from
    // shell. Idempotent.
    void rebind(Owner& new_owner) noexcept { owner_ = &new_owner; }

    // ── Mutation helpers for arithmetic types ──
    // Pre-increment: bump and invalidate. Returns the new value by
    // value (NOT a reference into Tracked's storage — that would let
    // the caller write through the read and skip the invalidator).
    template <class U = T,
              class = std::enable_if_t<std::is_arithmetic_v<U>>>
    T operator++() noexcept {
        ++value_;
        Invalidator{}(*owner_);
        return value_;
    }

    // Post-increment.
    template <class U = T,
              class = std::enable_if_t<std::is_arithmetic_v<U>>>
    T operator++(int) noexcept {
        T old = value_;
        ++value_;
        Invalidator{}(*owner_);
        return old;
    }

private:
    Owner* owner_;
    [[no_unique_address]] Invalidator inv_{};  // EBO: 0 B for empty stateless invalidator
    T value_;
};

} // namespace maya
