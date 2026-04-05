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

Element Divider(DividerProps props = {});

} // namespace maya::components
