#include "maya/components/disclosure.hpp"

namespace maya::components {

Element Disclosure::render(Children children) const {
    using namespace maya::dsl;

    // Header: chevron + title + optional badge
    const char* chevron = expanded_ ? "\u25be" : "\u25b8";

    std::vector<Element> header;
    header.push_back(text(chevron, Style{}.with_fg(palette().muted)));
    header.push_back(text(" " + props_.title,
                           Style{}.with_bold().with_fg(props_.color)));

    if (!props_.badge.empty()) {
        header.push_back(text(" " + props_.badge,
                               Style{}.with_fg(palette().muted)));
    }

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    if (expanded_) {
        // Indent children
        for (auto& child : children) {
            rows.push_back(
                hstack()(
                    text("  ", Style{}),
                    std::move(child)
                )
            );
        }
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
