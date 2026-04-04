#pragma once
// maya::components::Chip — Small badge/tag label
//
//   Chip({.label = "v2.1"})
//   Chip({.label = "Latest", .color = palette().success})
//   Chip({.label = "Error", .severity = Severity::Error})
//
// maya::components::Badge — Numeric count badge
//
//   Badge({.count = 3})
//   Badge({.count = 42, .color = palette().error})

#include "core.hpp"

namespace maya::components {

struct ChipProps {
    std::string label;
    Color       color = palette().muted;
    bool        bold  = false;
    // If set, overrides color with severity color
    std::optional<Severity> severity = std::nullopt;
};

inline Element Chip(ChipProps props) {
    using namespace maya::dsl;

    Color c = props.severity ? severity_color(*props.severity) : props.color;
    Style s = Style{}.with_fg(c);
    if (props.bold) s = s.with_bold();

    return text("[" + props.label + "]", s);
}

struct BadgeProps {
    int   count = 0;
    Color color = palette().primary;
};

inline Element Badge(BadgeProps props) {
    using namespace maya::dsl;

    if (props.count <= 0) return text("");

    auto label = props.count > 99 ? "99+" : std::to_string(props.count);
    return text(" " + label + " ",
                Style{}.with_bold().with_fg(Color::rgb(14, 14, 22)).with_bg(props.color));
}

} // namespace maya::components
