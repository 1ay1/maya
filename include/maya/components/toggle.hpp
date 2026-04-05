#pragma once
// maya::components::Toggle — Stateless toggle/checkbox component
//
//   Toggle({.checked = true, .label = "Dark mode"})
//   Toggle({.checked = false, .style = ToggleStyle::Switch})
//   Toggle({.checked = true, .style = ToggleStyle::Dot, .label = "Active"})

#include "core.hpp"

namespace maya::components {

enum class ToggleStyle { Checkbox, Switch, Dot };

struct ToggleProps {
    bool        checked  = false;
    std::string label    = "";
    ToggleStyle style    = ToggleStyle::Checkbox;
    Color       color    = {};  // default = palette().primary at render time
    bool        disabled = false;
};

Element Toggle(ToggleProps props = {});

} // namespace maya::components
