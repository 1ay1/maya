#pragma once
// maya::components::StatusBar — Bottom status bar with sections
//
//   StatusBar({.sections = {
//       {.content = "claude-opus-4-6", .color = palette().primary},
//       {.content = "1.2k tokens", .color = palette().muted},
//       {.content = "Ready", .color = palette().success},
//   }})
//
//   StatusBar({.left = "maya agent",
//              .center = "claude-opus-4-6",
//              .right = "[q]uit [t]heme"})

#include "core.hpp"

namespace maya::components {

struct StatusSection {
    std::string content;
    Color       color = palette().muted;
    bool        bold  = false;
};

struct StatusBarProps {
    // Simple mode: left/center/right text
    std::string left   = "";
    std::string center = "";
    std::string right  = "";

    // Advanced mode: arbitrary sections with separators
    std::vector<StatusSection> sections = {};

    Color bg        = Color::rgb(14, 14, 22);
    Color separator = palette().dim;
};

Element StatusBar(StatusBarProps props);

} // namespace maya::components
