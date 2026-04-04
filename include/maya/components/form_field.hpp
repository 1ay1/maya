#pragma once
// maya::components::FormField — Label + description + validation wrapper for inputs
//
//   FormField({.label = "Username", .required = true,
//              .error = "Username is already taken",
//              .children = { text_input.render() }})

#include "core.hpp"

namespace maya::components {

struct FormFieldProps {
    std::string label       = "";
    std::string description = "";
    std::string error       = "";
    bool        required    = false;
    Children    children    = {};
};

inline Element FormField(FormFieldProps props) {
    using namespace maya::dsl;

    std::vector<Element> rows;

    // Label row: bold label + optional required marker
    if (!props.label.empty()) {
        std::vector<Element> label_parts;
        label_parts.push_back(text(props.label, Style{}.with_bold().with_fg(palette().text)));
        if (props.required) {
            label_parts.push_back(text(" *", Style{}.with_bold().with_fg(palette().error)));
        }
        rows.push_back(hstack()(std::move(label_parts)));
    }

    // Description
    if (!props.description.empty()) {
        rows.push_back(text(props.description, Style{}.with_dim().with_italic()));
    }

    // Wrapped input children
    for (auto& child : props.children) {
        rows.push_back(std::move(child));
    }

    // Validation error
    if (!props.error.empty()) {
        rows.push_back(
            text("✗ " + props.error, Style{}.with_fg(palette().error))
        );
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
