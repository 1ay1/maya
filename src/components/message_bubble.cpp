#include "maya/components/message_bubble.hpp"

namespace maya::components {

Element MessageBubble(MessageBubbleProps props) {
    using namespace maya::dsl;

    auto& p = palette();

    // Role-specific styling
    const char* label;
    Color label_color, border_color;
    switch (props.role) {
        case Role::User:
            label = "You";
            label_color = p.accent;
            border_color = p.accent;
            break;
        case Role::Assistant:
            label = "Assistant";
            label_color = p.primary;
            border_color = props.is_streaming ? p.primary : p.border;
            break;
        case Role::System:
            label = "System";
            label_color = p.warning;
            border_color = p.warning;
            break;
    }

    // Header: role label + optional timestamp
    std::vector<Element> header_parts;
    header_parts.push_back(text(label, Style{}.with_bold().with_fg(label_color)));

    if (props.is_streaming) {
        header_parts.push_back(text(" "));
        header_parts.push_back(text(spin(props.frame), Style{}.with_fg(p.primary)));
    }

    if (!props.timestamp.empty()) {
        header_parts.push_back(Element(space));
        header_parts.push_back(text(props.timestamp, Style{}.with_fg(p.dim)));
    }

    std::vector<Element> body;
    body.push_back(hstack()(std::move(header_parts)));

    // Content
    if (!props.content.empty()) {
        body.push_back(text(props.content, Style{}.with_fg(p.text)));
    }

    for (auto& child : props.children) {
        body.push_back(std::move(child));
    }

    // Streaming cursor at the end
    if (props.is_streaming && props.content.empty() && props.children.empty()) {
        body.push_back(text("█", Style{}.with_fg(p.primary)));
    }

    auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(border_color)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(body));
}

} // namespace maya::components
