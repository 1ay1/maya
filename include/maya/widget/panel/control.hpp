#pragma once
// maya::panel::Control — the closed set of item kinds, one widget file each.
//
// EVERY kind is one self-contained widget in item/: its value struct and its
// renderer, side by side. Each renderer sees only its own value + ItemCtx
// (theme, dropdown-open, edit budget) — never the Config or the row list —
// which is the modularity guarantee: an item widget physically cannot grow
// panel logic.
//
// The variant is the ONE list. render(Control, ctx) dispatches by overload
// resolution, so a new kind without a renderer is a compile error inside
// std::visit, not a silently blank cell.

#include <concepts>
#include <string>
#include <utility>
#include <variant>

#include "../../element/element.hpp"
#include "../../element/text.hpp"

#include "item/action.hpp"
#include "item/choice.hpp"
#include "item/header.hpp"
#include "item/label.hpp"
#include "item/meter.hpp"
#include "item/number.hpp"
#include "item/path.hpp"
#include "item/pick.hpp"
#include "item/secret.hpp"
#include "item/slider.hpp"
#include "item/spark.hpp"
#include "item/text.hpp"
#include "item/toggle.hpp"

namespace maya::panel {

// Meter and Spark are the READ-ONLY drawn kinds — a proportion and a
// series. They are two kinds rather than one because their width rules are
// opposites: a meter fills its budget, a spark is one cell per sample and
// treats the budget as a window. Sharing a width is what once padded a
// series with blank until it pushed the value column off the row.
using Control = std::variant<Label, Header, Toggle, Choice, Pick, Number,
                             Slider, Text, Secret, Path, Action,
                             Meter, Spark>;

[[nodiscard]] inline bool is_header(const Control& c) noexcept {
    return std::holds_alternative<Header>(c);
}

// Is this control being EDITED right now (a live caret)? Text-like kinds
// only — everything else answers false. Lets the panel derive mode chrome
// (the footer hint, the row wash) from the ONE fact the host already
// supplies, instead of every host restating "I am editing" a second way.
[[nodiscard]] inline bool is_editing(const Control& c) noexcept {
    return std::visit(
        [](const auto& v) {
            if constexpr (requires { v.caret; })
                return v.caret != std::string::npos;
            else
                return false;
        },
        c);
}

[[nodiscard]] inline std::pair<std::string, Style>
render(const Control& c, const ItemCtx& ctx) {
    return std::visit([&](const auto& v) { return render(v, ctx); }, c);
}

// ── The Element channel ────────────────────────────────────────────
//
// render() answers with a STRING, and a string is one cell. It cannot
// participate in layout, so any kind that wants columns has to build them
// by concatenation — pad to right-align, shrink a bar in a loop to fit,
// blank-fill a strip that came up short. That hand-rolled flex is where
// every sizing bug in this family came from, and each instance is wrong in
// its own way because each is written separately.
//
// A kind can now OPT IN to laying itself out instead: define
//
//     Element render_element(const Kind&, const ItemCtx&);
//
// and the panel uses it, handing the result to maya's flex engine — which
// already does growing, shedding, min/max clamping and alignment, and is
// already tested. `grow(1)` gets the slack; `basis` aligns rows against a
// shared column; a sibling's cell cannot be eaten because the engine
// allocates before anything renders.
//
// Detected by expression SFINAE rather than a virtual or a flag, so:
//   • a kind that does not define it is UNCHANGED — all thirteen existing
//     kinds compile and render exactly as before, which is what makes this
//     additive rather than a thirteen-file breaking change;
//   • a kind that does define it needs no registration anywhere, so the
//     variant stays the one list it claims to be.
template <class T>
concept LaysItselfOut = requires(const T& v, const ItemCtx& ctx) {
    { render_element(v, ctx) } -> std::same_as<Element>;
};

// Does this control lay itself out? Asked by the panel before it builds a
// string cell it may not need.
[[nodiscard]] inline bool lays_itself_out(const Control& c) noexcept {
    return std::visit([](const auto& v) {
        return LaysItselfOut<std::remove_cvref_t<decltype(v)>>;
    }, c);
}

// The laid-out cell. Only valid when lays_itself_out(c); the string path
// is the answer otherwise.
//
// Named differently from the per-kind overloads on purpose. Calling this
// `render_element` too made the visitor's `render_element(v, ctx)` resolve
// back to THIS function — Control is implicitly constructible from any
// alternative, so the variant overload was always viable — and the result
// was unbounded recursion that segfaulted the moment any panel built a
// row. Overload sets that include a converting constructor from their own
// argument type are a trap; a distinct name is the cheap way out.
[[nodiscard]] inline Element control_element(const Control& c,
                                             const ItemCtx& ctx) {
    return std::visit([&](const auto& v) -> Element {
        if constexpr (LaysItselfOut<std::remove_cvref_t<decltype(v)>>)
            return render_element(v, ctx);
        else
            return Element{TextElement{}};
    }, c);
}

} // namespace maya::panel

namespace maya {
// Compatibility spelling — the alias predates the panel/ folder.
using PanelControl = panel::Control;
} // namespace maya
