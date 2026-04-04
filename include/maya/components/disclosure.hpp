#pragma once
// maya::components::Disclosure — Collapsible section with chevron toggle
//
//   Disclosure disclosure({.title = "Details", .expanded = true});
//
//   // Toggle on key:
//   disclosure.toggle();
//
//   // Render:
//   disclosure.render({ text("Inner content"), text("More...") })

#include "core.hpp"

namespace maya::components {

struct DisclosureProps {
    std::string title    = "";
    bool        expanded = false;
    Color       color    = palette().text;
    std::string badge    = "";   // optional count/badge text after title
};

class Disclosure {
    bool expanded_;
    DisclosureProps props_;

public:
    explicit Disclosure(DisclosureProps props = {})
        : expanded_(props.expanded), props_(std::move(props)) {}

    [[nodiscard]] bool expanded() const { return expanded_; }
    void toggle() { expanded_ = !expanded_; }
    void expand() { expanded_ = true; }
    void collapse() { expanded_ = false; }
    void set_expanded(bool v) { expanded_ = v; }

    [[nodiscard]] Element render(Children children = {}) const {
        using namespace maya::dsl;

        // Header: chevron + title + optional badge
        const char* chevron = expanded_ ? "▾" : "▸";

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
};

} // namespace maya::components
