#pragma once
// maya::components::DiffView — Unified diff display with colored +/- lines
//
//   DiffView({.diff = unified_diff_string})
//   DiffView({.diff = diff_output, .file_path = "src/main.cpp"})
//
// Renders unified diff format with:
// - Green lines for additions (+)
// - Red lines for deletions (-)
// - Blue lines for hunk headers (@@ ... @@)
// - Dim lines for context
// - File header styling

#include "core.hpp"

namespace maya::components {

struct DiffViewProps {
    std::string diff;
    std::string file_path    = "";
    bool        show_header  = true;
    Color       add_fg       = palette().diff_add;
    Color       del_fg       = palette().diff_del;
    Color       hunk_fg      = palette().info;
    Color       context_fg   = palette().muted;
    Color       add_bg       = Color::rgb(20, 40, 25);
    Color       del_bg       = Color::rgb(45, 20, 20);
};

Element DiffView(DiffViewProps props);

} // namespace maya::components
