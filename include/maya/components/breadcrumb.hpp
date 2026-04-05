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

Element Breadcrumb(BreadcrumbProps props);

Element FileBreadcrumb(const std::string& path, Color color = {});

} // namespace maya::components
