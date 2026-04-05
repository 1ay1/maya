#pragma once
// maya::components::GitGraph — ASCII git commit graph with branch lines
//
//   GitGraph({.commits = {{"3a2f1c8", "Fix auth token expiry", "", "2m ago"}}})
//   GitGraph({.commits = log, .show_author = true, .max_branches = 6})
//
// Renders a vertical commit graph with branch lines, merge points,
// and colored columns for parallel branches.

#include "core.hpp"

namespace maya::components {

struct GitCommit {
    std::string hash;         // short hash (7 chars)
    std::string message;
    std::string author   = "";
    std::string time     = "";
    int         branch   = 0;  // branch column (0 = main, 1 = first branch, etc.)
    bool        is_merge = false;
    bool        is_head  = false;
};

struct GitGraphProps {
    std::vector<GitCommit> commits = {};
    int  max_branches = 4;        // max parallel branches to show
    bool show_hash    = true;
    bool show_author  = false;
    bool show_time    = true;
};

Element GitGraph(GitGraphProps props = {});

} // namespace maya::components
