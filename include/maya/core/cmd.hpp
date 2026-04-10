#pragma once
// maya::Cmd<Msg> - Side effects as data
//
// The core type-theoretic insight: a pure function cannot perform I/O.
// But it CAN return a *description* of I/O to be performed later.
// Cmd<Msg> is that description.
//
// An update function receives the current state and an event, and returns
// the new state alongside a Cmd describing what should happen next:
//
//   auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
//       return std::visit(overload{
//           [&](Save) { return {m, Cmd<Msg>::write_clipboard(m.text)}; },
//           [&](Quit) { return {m, Cmd<Msg>::quit()}; },
//       }, msg);
//   }
//
// This makes update() a pure function — same inputs, same outputs,
// testable with operator==. The runtime interprets the Cmd and performs
// the actual terminal I/O.
//
// Cmd is an algebraic data type (std::variant) following the same pattern
// as Element, Event, and RenderOp elsewhere in maya. Each alternative
// describes one kind of effect. Smart constructors provide a clean API.
// The functor map() operation allows child components to embed their
// local Msg type into a parent's.

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "overload.hpp"

namespace maya {

// ============================================================================
// Cmd<Msg> - Algebraic description of a side effect
// ============================================================================
// A sum type: exactly one of the alternatives below. The runtime
// pattern-matches on the variant and performs the effect. User code
// never performs effects directly — it returns Cmd values.
//
// This is maya's equivalent of Haskell's IO monad or Rust's Command:
// a value that *describes* an effect without *performing* it.

template <typename Msg>
class Cmd {
public:

    // -- Alternatives (each describes one kind of effect) ---------------------

    struct None {};
    struct Quit {};

    struct Batch {
        std::vector<Cmd> cmds;
    };

    struct After {
        std::chrono::milliseconds delay;
        Msg                       msg;
    };

    struct SetTitle {
        std::string title;
    };

    struct WriteClipboard {
        std::string content;
    };

    /// Escape hatch: arbitrary async work. The function receives a dispatch
    /// callback and calls it with a Msg when the result is ready.
    struct Task {
        std::function<void(std::function<void(Msg)>)> run;
    };

    using Variant = std::variant<None, Quit, Batch, After,
                                 SetTitle, WriteClipboard, Task>;
    Variant inner;

    // -- Smart constructors ---------------------------------------------------

    [[nodiscard]] static constexpr auto none() -> Cmd { return {None{}}; }
    [[nodiscard]] static constexpr auto quit() -> Cmd { return {Quit{}}; }

    [[nodiscard]] static auto after(std::chrono::milliseconds d, Msg m) -> Cmd {
        return {After{d, std::move(m)}};
    }

    [[nodiscard]] static auto set_title(std::string t) -> Cmd {
        return {SetTitle{std::move(t)}};
    }

    [[nodiscard]] static auto write_clipboard(std::string s) -> Cmd {
        return {WriteClipboard{std::move(s)}};
    }

    template <std::invocable<std::function<void(Msg)>> F>
    [[nodiscard]] static auto task(F&& f) -> Cmd {
        return {Task{std::forward<F>(f)}};
    }

    /// Combine multiple Cmds. Flattens nested batches and strips Nones.
    [[nodiscard]] static auto batch(std::vector<Cmd> cmds) -> Cmd {
        std::vector<Cmd> flat;
        flat.reserve(cmds.size());
        for (auto& c : cmds) {
            if (std::holds_alternative<None>(c.inner)) continue;
            if (auto* b = std::get_if<Batch>(&c.inner)) {
                for (auto& inner : b->cmds)
                    flat.push_back(std::move(inner));
            } else {
                flat.push_back(std::move(c));
            }
        }
        if (flat.empty()) return none();
        if (flat.size() == 1) return std::move(flat[0]);
        return {Batch{std::move(flat)}};
    }

    template <typename... Cmds>
        requires (sizeof...(Cmds) > 1 && (std::same_as<std::remove_cvref_t<Cmds>, Cmd> && ...))
    [[nodiscard]] static auto batch(Cmds&&... cmds) -> Cmd {
        std::vector<Cmd> v;
        v.reserve(sizeof...(Cmds));
        (v.push_back(std::forward<Cmds>(cmds)), ...);
        return batch(std::move(v));
    }

    // -- Functor map ----------------------------------------------------------
    // Transform the Msg type. If you have a child component with its own
    // Msg type, map() lets you embed its Cmds into the parent's Cmd<ParentMsg>.
    //
    //   Cmd<ChildMsg> child_cmd = child_update(child_model, child_msg);
    //   Cmd<ParentMsg> parent_cmd = child_cmd.map([](ChildMsg m) {
    //       return ParentMsg{ChildEvent{std::move(m)}};
    //   });

    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F&& f) const -> Cmd<std::invoke_result_t<F, Msg>> {
        using B = std::invoke_result_t<F, Msg>;
        return std::visit(overload{
            [](const None&)           -> Cmd<B> { return Cmd<B>::none(); },
            [](const Quit&)           -> Cmd<B> { return Cmd<B>::quit(); },
            [&](const After& a)       -> Cmd<B> { return Cmd<B>::after(a.delay, f(a.msg)); },
            [](const SetTitle& s)     -> Cmd<B> { return Cmd<B>::set_title(s.title); },
            [](const WriteClipboard& w) -> Cmd<B> { return Cmd<B>::write_clipboard(w.content); },
            [&](const Batch& b)       -> Cmd<B> {
                std::vector<Cmd<B>> mapped;
                mapped.reserve(b.cmds.size());
                for (auto& c : b.cmds)
                    mapped.push_back(c.map(f));
                return Cmd<B>::batch(std::move(mapped));
            },
            [&](const Task& t) -> Cmd<B> {
                return Cmd<B>::task(
                    [run = t.run, mapper = std::forward<F>(f)]
                    (std::function<void(B)> dispatch) {
                        run([&mapper, &dispatch](Msg m) {
                            dispatch(mapper(std::move(m)));
                        });
                    });
            },
        }, inner);
    }

    // -- Queries --------------------------------------------------------------

    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(inner);
    }

    [[nodiscard]] bool is_quit() const noexcept {
        return std::holds_alternative<Quit>(inner);
    }
};

} // namespace maya
