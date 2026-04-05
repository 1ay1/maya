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

    [[nodiscard]] Element render(Children children = {}) const;
};

} // namespace maya::components
