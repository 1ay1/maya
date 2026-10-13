#pragma once
// maya::core::function — MoveOnlyFunction<Sig> portability alias
//
// std::move_only_function is a C++23 <functional> LIBRARY feature: libstdc++
// has shipped it since GCC 12, but libc++ (Android/Termux clang, older Apple
// toolchains) gained it much later, so `-std=c++23` alone does not guarantee
// it exists. Widgets store move-only callbacks (captures routinely hold
// unique_ptrs / socket handles), so std::function is not a substitute — it
// requires copyable targets.
//
// When the standard type is available we alias it. Otherwise we fall back to
// a minimal unique_ptr type-erased wrapper covering exactly the surface this
// codebase uses: construct from any callable, move-only, operator(),
// explicit operator bool, assign/compare nullptr. Only the plain
// `R(Args...)` signature form is provided (no const/ref/noexcept qualified
// signatures — nothing in the tree needs them).

#include <version>

#if defined(__cpp_lib_move_only_function)

#include <functional>

namespace maya {
template <class Sig>
using MoveOnlyFunction = std::move_only_function<Sig>;
} // namespace maya

#else // fallback for standard libraries without std::move_only_function

#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace maya {

template <class Sig>
class MoveOnlyFunction; // only the plain R(Args...) form is provided

template <class R, class... Args>
class MoveOnlyFunction<R(Args...)> {
    struct Callable {
        virtual ~Callable() = default;
        virtual R invoke(Args&&... args) = 0;
    };
    template <class F>
    struct Model final : Callable {
        F fn;
        explicit Model(F f) : fn(std::move(f)) {}
        R invoke(Args&&... args) override {
            // static_cast<R> also handles R = void (discards the value) and
            // callables whose return type merely converts to R.
            return static_cast<R>(fn(std::forward<Args>(args)...));
        }
    };

    std::unique_ptr<Callable> impl_;

public:
    MoveOnlyFunction() noexcept = default;
    MoveOnlyFunction(std::nullptr_t) noexcept {}

    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, MoveOnlyFunction> &&
                 std::invocable<std::decay_t<F>&, Args...>)
    MoveOnlyFunction(F&& f)
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept            = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction(const MoveOnlyFunction&)                = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&)     = delete;

    MoveOnlyFunction& operator=(std::nullptr_t) noexcept {
        impl_.reset();
        return *this;
    }

    // Precondition (same as std::move_only_function): *this holds a target.
    R operator()(Args... args) {
        return impl_->invoke(std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return impl_ != nullptr;
    }

    void swap(MoveOnlyFunction& other) noexcept { impl_.swap(other.impl_); }

    friend bool operator==(const MoveOnlyFunction& f, std::nullptr_t) noexcept {
        return f.impl_ == nullptr;
    }
};

} // namespace maya

#endif // __cpp_lib_move_only_function
