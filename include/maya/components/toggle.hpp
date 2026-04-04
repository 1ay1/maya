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

inline Element Toggle(ToggleProps props) {
    using namespace maya::dsl;

    auto& p = palette();
    auto is_default = [](Color c) {
        return c.kind() == Color::Kind::Named && c.r() == 7;
    };
    Color c = props.disabled ? p.dim : (is_default(props.color) ? p.primary : props.color);
    Color dim = p.dim;

    std::vector<Element> parts;

    switch (props.style) {
    case ToggleStyle::Checkbox:
        if (props.checked)
            parts.push_back(text("[✓]", Style{}.with_fg(c)));
        else
            parts.push_back(text("[ ]", Style{}.with_fg(dim)));
        break;

    case ToggleStyle::Switch:
        if (props.checked)
            parts.push_back(text("(●∙∙)", Style{}.with_fg(c)));
        else
            parts.push_back(text("(∙∙●)", Style{}.with_fg(dim)));
        break;

    case ToggleStyle::Dot:
        if (props.checked)
            parts.push_back(text("●", Style{}.with_fg(c)));
        else
            parts.push_back(text("○", Style{}.with_fg(dim)));
        break;
    }

    if (!props.label.empty()) {
        Color label_color = props.disabled ? dim : p.text;
        parts.push_back(text(" " + props.label, Style{}.with_fg(label_color)));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
