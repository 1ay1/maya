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

#include <string>
#include <utility>
#include <variant>

#include "item/action.hpp"
#include "item/choice.hpp"
#include "item/header.hpp"
#include "item/label.hpp"
#include "item/number.hpp"
#include "item/path.hpp"
#include "item/pick.hpp"
#include "item/secret.hpp"
#include "item/slider.hpp"
#include "item/text.hpp"
#include "item/toggle.hpp"

namespace maya::panel {

using Control = std::variant<Label, Header, Toggle, Choice, Pick, Number,
                             Slider, Text, Secret, Path, Action>;

[[nodiscard]] inline bool is_header(const Control& c) noexcept {
    return std::holds_alternative<Header>(c);
}

[[nodiscard]] inline std::pair<std::string, Style>
render(const Control& c, const ItemCtx& ctx) {
    return std::visit([&](const auto& v) { return render(v, ctx); }, c);
}

} // namespace maya::panel

namespace maya {
// Compatibility spelling — the alias predates the panel/ folder.
using PanelControl = panel::Control;
} // namespace maya
