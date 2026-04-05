#include "maya/components/form_field.hpp"

namespace maya::components {

Element FormField(FormFieldProps props) {
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
            text("\xe2\x9c\x97 " + props.error, Style{}.with_fg(palette().error))
        );
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
