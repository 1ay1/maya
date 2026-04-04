#pragma once
// maya::components::Breadcrumb — Navigation breadcrumb trail
//
//   Breadcrumb({.items = {{"src"}, {"middleware"}, {"auth.ts", "", true}}})
//   Breadcrumb({.items = items, .separator = " / "})
//   FileBreadcrumb("src/middleware/auth.ts")

#include "core.hpp"

namespace maya::components {

struct BreadcrumbItem {
    std::string label;
    std::string icon   = "";
    bool        active = false;
};

struct BreadcrumbProps {
    std::vector<BreadcrumbItem> items           = {};
    std::string                 separator       = " › ";
    Color                       active_color    = {};
    Color                       inactive_color  = {};
    Color                       separator_color = {};
};

inline Element Breadcrumb(BreadcrumbProps props) {
    using namespace maya::dsl;

    auto& p = palette();
    auto is_default = [](Color c) {
        return c.kind() == Color::Kind::Named && c.r() == 7;
    };
    Color act_c   = is_default(props.active_color)    ? p.primary : props.active_color;
    Color inact_c = is_default(props.inactive_color)  ? p.muted   : props.inactive_color;
    Color sep_c   = is_default(props.separator_color)  ? p.dim     : props.separator_color;

    std::vector<Element> parts;
    for (size_t i = 0; i < props.items.size(); ++i) {
        const auto& item = props.items[i];

        // Separator before non-first items
        if (i > 0) {
            parts.push_back(text(props.separator, Style{}.with_dim().with_fg(sep_c)));
        }

        // Build label with optional icon
        std::string label = item.icon.empty() ? item.label : item.icon + " " + item.label;

        if (item.active) {
            parts.push_back(text(label, Style{}.with_bold().with_fg(act_c)));
        } else {
            parts.push_back(text(label, Style{}.with_fg(inact_c)));
        }
    }

    return hstack()(std::move(parts));
}

inline Element FileBreadcrumb(const std::string& path, Color color = {}) {
    using namespace maya::dsl;

    std::vector<BreadcrumbItem> items;
    std::string segment;
    for (char ch : path) {
        if (ch == '/') {
            if (!segment.empty()) {
                items.push_back({segment});
                segment.clear();
            }
        } else {
            segment += ch;
        }
    }
    if (!segment.empty()) {
        items.push_back({segment});
    }

    // Mark last item as active
    if (!items.empty()) {
        items.back().active = true;
    }

    BreadcrumbProps props{.items = std::move(items)};
    auto is_default = [](Color c) {
        return c.kind() == Color::Kind::Named && c.r() == 7;
    };
    if (!is_default(color)) {
        props.active_color = color;
    }

    return Breadcrumb(std::move(props));
}

} // namespace maya::components
