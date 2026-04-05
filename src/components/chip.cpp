#include "maya/components/chip.hpp"

namespace maya::components {

Element Chip(ChipProps props) {
    using namespace maya::dsl;

    Color c = props.severity ? severity_color(*props.severity) : props.color;
    Style s = Style{}.with_fg(c);
    if (props.bold) s = s.with_bold();

    return text("[" + props.label + "]", s);
}

Element Badge(BadgeProps props) {
    using namespace maya::dsl;

    if (props.count <= 0) return text("");

    auto label = props.count > 99 ? "99+" : std::to_string(props.count);
    return text(" " + label + " ",
                Style{}.with_bold().with_fg(Color::rgb(14, 14, 22)).with_bg(props.color));
}

} // namespace maya::components
