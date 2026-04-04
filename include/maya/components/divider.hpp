#pragma once
// maya::components::Divider — Horizontal or vertical separator
//
//   Divider()                              // thin horizontal line
//   Divider({.label = "Section"})          // labeled divider
//   Divider({.style = DividerStyle::Thick})
//   Divider({.vertical = true})

#include "core.hpp"

namespace maya::components {

enum class DividerStyle { Thin, Thick, Dashed, Double };

struct DividerProps {
    std::string  label    = "";
    DividerStyle style    = DividerStyle::Thin;
    Color        color    = palette().border;
    bool         vertical = false;
};

inline Element Divider(DividerProps props = {}) {
    using namespace maya::dsl;

    auto border_style = [&]() -> BorderStyle {
        switch (props.style) {
            case DividerStyle::Thin:   return BorderStyle::Single;
            case DividerStyle::Thick:  return BorderStyle::Bold;
            case DividerStyle::Dashed: return BorderStyle::Single;
            case DividerStyle::Double: return BorderStyle::Double;
        }
        return BorderStyle::Single;
    }();

    if (props.vertical) {
        auto sides = BorderSides{.top = false, .right = true, .bottom = false, .left = false};
        return vstack()
            .border(border_style)
            .border_color(props.color)
            .border_sides(sides);
    }

    if (props.label.empty()) {
        auto sides = BorderSides{.top = false, .right = false, .bottom = true, .left = false};
        return vstack()
            .border(border_style)
            .border_color(props.color)
            .border_sides(sides);
    }

    // Labeled divider: ── Label ──────────────
    auto sides = BorderSides{.top = true, .right = false, .bottom = false, .left = false};
    return hstack()
        .border(BorderStyle::Single)
        .border_color(props.color)
        .border_sides(sides)
        .border_text(props.label, BorderTextPos::Top, BorderTextAlign::Start);
}

} // namespace maya::components
