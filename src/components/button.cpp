#include "maya/components/button.hpp"

namespace maya::components {

Element Button(ButtonProps props) {
    using namespace maya::dsl;

    Color fg = props.disabled ? palette().dim : props.color;
    Style label_style = Style{}.with_fg(fg);

    if (props.variant == Variant::Filled || props.active) {
        label_style = label_style.with_bold().with_inverse();
    } else if (props.variant == Variant::Tinted) {
        label_style = label_style.with_bold();
    } else if (props.variant == Variant::Ghost) {
        label_style = Style{}.with_fg(props.disabled ? palette().dim : palette().muted);
    }

    std::vector<Element> parts;

    // Icon
    if (!props.icon.empty()) {
        parts.push_back(text(props.icon, Style{}.with_fg(fg)));
    }

    // Label with padding
    std::string padded;
    if (props.variant == Variant::Filled || props.variant == Variant::Outlined) {
        padded = " " + props.label + " ";
    } else {
        padded = props.label;
    }
    parts.push_back(text(std::move(padded), label_style));

    // Keyhint
    if (!props.keyhint.empty()) {
        parts.push_back(text(" " + props.keyhint,
                             Style{}.with_dim().with_fg(palette().muted)));
    }

    auto builder = hstack().gap(0);
    if (props.variant == Variant::Outlined) {
        builder = std::move(builder).border(BorderStyle::Round).border_color(fg);
    }

    return builder(std::move(parts));
}

} // namespace maya::components
