#pragma once
// maya::Sub<Msg> - Declarative event subscriptions
//
// Sub describes WHAT events the application cares about as a function
// of the current state. The runtime diffs subscriptions between frames,
// starting and stopping listeners as needed.
//
// This is the input side of "Output = Input + Effect":
//   - Cmd<Msg>  describes effects going OUT  (terminal writes, clipboard, timers)
//   - Sub<Msg>  describes events coming IN   (keys, mouse, resize, timers)
//
// Both are data — descriptions, not actions. The runtime interprets them.
//
//   static auto subscribe(const Model& m) -> Sub<Msg> {
//       auto subs = Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> {
//           if (key_is(k, 'q')) return Quit{};
//           if (key_is(k, '+')) return Increment{};
//           return std::nullopt;   // not interested in this key
//       });
//       if (m.animating)
//           subs = Sub<Msg>::batch(std::move(subs), Sub<Msg>::every(16ms, Tick{}));
//       return subs;
//   }

#include <chrono>
#include <functional>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "../core/overload.hpp"
#include "../core/types.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// Sub<Msg> - Algebraic description of event sources
// ============================================================================
// A sum type. Each alternative describes one kind of event source.
// The runtime pattern-matches on it to set up listeners.

template <typename Msg>
class Sub {
public:

    // -- Alternatives ---------------------------------------------------------

    struct None {};

    struct Batch {
        std::vector<Sub> subs;
    };

    /// Filter keyboard events into messages.
    /// Return std::nullopt to ignore the event.
    struct OnKey {
        std::function<std::optional<Msg>(const KeyEvent&)> filter;
    };

    /// Filter mouse events into messages.
    struct OnMouse {
        std::function<std::optional<Msg>(const MouseEvent&)> filter;
    };

    /// Map resize events into messages.
    struct OnResize {
        std::function<Msg(Size)> on_resize;
    };

    /// Map paste events into messages.
    struct OnPaste {
        std::function<Msg(std::string)> on_paste;
    };

    /// Emit a message at a fixed interval (for animations, polling, etc.).
    struct Every {
        std::chrono::milliseconds interval;
        Msg                       msg;
    };

    using Variant = std::variant<None, Batch, OnKey, OnMouse,
                                 OnResize, OnPaste, Every>;
    Variant inner;

    // -- Smart constructors ---------------------------------------------------

    [[nodiscard]] static constexpr auto none() -> Sub { return {None{}}; }

    /// Subscribe to keyboard events with a filter function.
    template <std::invocable<const KeyEvent&> F>
        requires std::same_as<std::invoke_result_t<F, const KeyEvent&>, std::optional<Msg>>
    [[nodiscard]] static auto on_key(F&& f) -> Sub {
        return {OnKey{std::forward<F>(f)}};
    }

    /// Subscribe to mouse events with a filter function.
    template <std::invocable<const MouseEvent&> F>
        requires std::same_as<std::invoke_result_t<F, const MouseEvent&>, std::optional<Msg>>
    [[nodiscard]] static auto on_mouse(F&& f) -> Sub {
        return {OnMouse{std::forward<F>(f)}};
    }

    /// Subscribe to terminal resize events.
    template <std::invocable<Size> F>
        requires std::convertible_to<std::invoke_result_t<F, Size>, Msg>
    [[nodiscard]] static auto on_resize(F&& f) -> Sub {
        return {OnResize{std::forward<F>(f)}};
    }

    /// Subscribe to paste events.
    template <std::invocable<std::string> F>
        requires std::convertible_to<std::invoke_result_t<F, std::string>, Msg>
    [[nodiscard]] static auto on_paste(F&& f) -> Sub {
        return {OnPaste{std::forward<F>(f)}};
    }

    /// Emit a message at a fixed interval.
    [[nodiscard]] static auto every(std::chrono::milliseconds interval, Msg msg) -> Sub {
        return {Every{interval, std::move(msg)}};
    }

    /// Combine multiple subscriptions.
    [[nodiscard]] static auto batch(std::vector<Sub> subs) -> Sub {
        std::vector<Sub> flat;
        flat.reserve(subs.size());
        for (auto& s : subs) {
            if (std::holds_alternative<None>(s.inner)) continue;
            if (auto* b = std::get_if<Batch>(&s.inner)) {
                for (auto& inner : b->subs)
                    flat.push_back(std::move(inner));
            } else {
                flat.push_back(std::move(s));
            }
        }
        if (flat.empty()) return none();
        if (flat.size() == 1) return std::move(flat[0]);
        return {Batch{std::move(flat)}};
    }

    template <typename... Subs>
        requires (sizeof...(Subs) > 1 && (std::same_as<std::remove_cvref_t<Subs>, Sub> && ...))
    [[nodiscard]] static auto batch(Subs&&... subs) -> Sub {
        std::vector<Sub> v;
        v.reserve(sizeof...(Subs));
        (v.push_back(std::forward<Subs>(subs)), ...);
        return batch(std::move(v));
    }

    // -- Functor map ----------------------------------------------------------

    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F&& f) const -> Sub<std::invoke_result_t<F, Msg>> {
        using B = std::invoke_result_t<F, Msg>;
        return std::visit(overload{
            [](const None&)  -> Sub<B> { return Sub<B>::none(); },
            [&](const OnKey& s) -> Sub<B> {
                auto filter = s.filter;
                auto mapper = f;
                return Sub<B>::on_key([filter = std::move(filter),
                                       mapper = std::move(mapper)](const KeyEvent& k)
                    -> std::optional<B> {
                        if (auto m = filter(k)) return mapper(std::move(*m));
                        return std::nullopt;
                    });
            },
            [&](const OnMouse& s) -> Sub<B> {
                auto filter = s.filter;
                auto mapper = f;
                return Sub<B>::on_mouse([filter = std::move(filter),
                                         mapper = std::move(mapper)](const MouseEvent& m)
                    -> std::optional<B> {
                        if (auto msg = filter(m)) return mapper(std::move(*msg));
                        return std::nullopt;
                    });
            },
            [&](const OnResize& s) -> Sub<B> {
                auto on_r = s.on_resize;
                auto mapper = f;
                return Sub<B>::on_resize([on_r = std::move(on_r),
                                          mapper = std::move(mapper)](Size sz) -> B {
                    return mapper(on_r(sz));
                });
            },
            [&](const OnPaste& s) -> Sub<B> {
                auto on_p = s.on_paste;
                auto mapper = f;
                return Sub<B>::on_paste([on_p = std::move(on_p),
                                         mapper = std::move(mapper)](std::string txt) -> B {
                    return mapper(on_p(std::move(txt)));
                });
            },
            [&](const Every& s) -> Sub<B> {
                return Sub<B>::every(s.interval, f(s.msg));
            },
            [&](const Batch& b) -> Sub<B> {
                std::vector<Sub<B>> mapped;
                mapped.reserve(b.subs.size());
                for (auto& s : b.subs)
                    mapped.push_back(s.map(f));
                return Sub<B>::batch(std::move(mapped));
            },
        }, inner);
    }

    // -- Queries --------------------------------------------------------------

    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(inner);
    }
};

} // namespace maya
