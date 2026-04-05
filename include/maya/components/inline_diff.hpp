#pragma once
// maya::components::InlineDiff — Word-level inline diff rendering
//
//   InlineDiff({.before = "const SESSION_TIMEOUT = 3600;",
//               .after  = "const TOKEN_EXPIRY = '1h';"})
//
// Renders two lines ("before:" / "after:") with word-level coloring:
// - Deleted words in red with strikethrough
// - Added words in green with bold
// - Unchanged words in normal text color
// Uses LCS (longest common subsequence) on whitespace-split tokens.

#include "core.hpp"

#include <algorithm>
#include <sstream>

namespace maya::components {

struct InlineDiffProps {
    std::string before;
    std::string after;
    std::string label       = "";
    Color       add_fg      = {};
    Color       del_fg      = {};
    Color       add_bg      = Color::rgb(20, 50, 30);
    Color       del_bg      = Color::rgb(50, 20, 20);
    Color       same_fg     = {};
    bool        show_header = true;
};

Element InlineDiff(InlineDiffProps props = {});

} // namespace maya::components
