#pragma once
// maya::scope_exit — RAII scope guard for deferred cleanup
//
// Runs a callable unconditionally when the scope exits. Equivalent to
// Boost.ScopeExit / GSL finally / C++26 std::scope_exit (P0052R10).
//
// Usage:
//   auto guard = scope_exit([&] { cleanup(); });
//   // ... guard runs cleanup() on scope exit (normal or exception)

#include <concepts>
#include <type_traits>
#include <utility>

namespace maya {

template <std::invocable F>
class [[nodiscard]] scope_exit {
    F fn_;
    bool active_ = true;

public:
    explicit scope_exit(F&& fn) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(fn)) {}

    explicit scope_exit(const F& fn) noexcept(std::is_nothrow_copy_constructible_v<F>)
        : fn_(fn) {}

    ~scope_exit() noexcept {
        if (active_) fn_();
    }

    /// Disarm: prevent the cleanup from running.
    void release() noexcept { active_ = false; }

    scope_exit(scope_exit&& o) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(o.fn_)), active_(std::exchange(o.active_, false)) {}

    scope_exit(const scope_exit&)            = delete;
    scope_exit& operator=(const scope_exit&) = delete;
    scope_exit& operator=(scope_exit&&)      = delete;
};

// Deduction guide
template <typename F>
scope_exit(F) -> scope_exit<F>;

} // namespace maya
