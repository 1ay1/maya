#pragma once
// maya::components::Callout — Info/success/warning/error notification box
//
//   Callout({.severity = Severity::Warning,
//            .title = "Deprecated API",
//            .body = "This endpoint will be removed in v3."})
//
//   Callout({.severity = Severity::Error,
//            .title = "Build failed",
//            .children = { text("Exit code 1"), text("See logs") }})

#include "core.hpp"

namespace maya::components {

struct CalloutProps {
    Severity    severity = Severity::Info;
    std::string title    = "";
    std::string body     = "";
    Children    children = {};
};

inline Element Callout(CalloutProps props) {
    using namespace maya::dsl;

    Color c = severity_color(props.severity);
    const char* icon = severity_icon(props.severity);

    std::vector<Element> content;

    // Title line: icon + title
    if (!props.title.empty()) {
        content.push_back(
            hstack().gap(1)(
                text(icon, Style{}.with_bold().with_fg(c)),
                text(props.title, Style{}.with_bold().with_fg(c))
            )
        );
    }

    // Body text
    if (!props.body.empty()) {
        content.push_back(
            text(props.body, Style{}.with_fg(palette().text))
        );
    }

    // Extra children
    for (auto& child : props.children) {
        content.push_back(std::move(child));
    }

    return vstack()
        .border(BorderStyle::Round)
        .border_color(c)
        .padding(0, 1, 0, 1)(std::move(content));
}

} // namespace maya::components
