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

Element Chip(ChipProps props);

struct BadgeProps {
    int   count = 0;
    Color color = palette().primary;
};

Element Badge(BadgeProps props);

} // namespace maya::components
