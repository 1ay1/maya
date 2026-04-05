#pragma once
// maya::components::ContextPill — @-mention context chips
//
//   ContextPill({.kind = ContextKind::File, .label = "src/main.cpp"})
//   ContextPill({.kind = ContextKind::Url, .label = "https://...", .removable = true})
//   ContextPillRow({.pills = {pill1_props, pill2_props}})

#include "core.hpp"

namespace maya::components {

enum class ContextKind {
    File, Directory, Symbol, Url, Selection, GitDiff, Diagnostics, Image, Thread, Rules
};

namespace detail {

const char* context_icon(ContextKind kind);


Color context_color(ContextKind kind);


} // namespace detail

struct ContextPillProps {
    ContextKind kind      = ContextKind::File;
    std::string label;
    bool        removable = false;
    Color       color     = Color::rgb(0, 0, 0);  // 0,0,0 = use default
};

Element ContextPill(ContextPillProps props);


struct ContextPillRowProps {
    std::vector<ContextPillProps> pills = {};
};

Element ContextPillRow(ContextPillRowProps props);


} // namespace maya::components
