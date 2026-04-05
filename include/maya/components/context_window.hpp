#pragma once
// maya::components::ContextWindow — AI context window usage visualization
//
//   ContextWindow({.segments = {{"System", 12400, palette().info},
//                               {"History", 89200, palette().secondary},
//                               {"Tools", 32100, palette().accent},
//                               {"Response", 11534, palette().success}},
//                  .max_tokens = 200000})

#include "core.hpp"

namespace maya::components {

struct ContextSegment {
    std::string label;      // e.g. "System", "History", "Tools", "Response"
    int         tokens = 0;
    Color       color  = {};  // default = palette().primary
};

struct ContextWindowProps {
    std::vector<ContextSegment> segments = {};
    int  max_tokens   = 200000;   // model's context limit
    int  width        = 40;       // bar width in chars
    bool show_labels  = true;     // show legend below
    bool show_percent = true;     // show % in bar
};

namespace detail {

bool is_default_color(Color c);

} // namespace detail

Element ContextWindow(ContextWindowProps props = {});

} // namespace maya::components
