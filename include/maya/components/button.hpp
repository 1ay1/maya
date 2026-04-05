#pragma once
// maya::components::Button — Styled text button
//
//   Button({.label = "Submit"})
//   Button({.label = "Cancel", .variant = Variant::Ghost})
//   Button({.label = "Delete", .variant = Variant::Filled, .color = palette().error})
//   Button({.label = "Run", .icon = "▶", .keyhint = "Enter"})

#include "core.hpp"

namespace maya::components {

struct ButtonProps {
    std::string label;
    std::string icon    = "";
    std::string keyhint = "";
    Variant     variant = Variant::Outlined;
    Color       color   = palette().primary;
    bool        active  = false;
    bool        disabled = false;
};

Element Button(ButtonProps props = {});

} // namespace maya::components
