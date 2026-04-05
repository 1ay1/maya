#pragma once
// maya::components::Waterfall — Timing waterfall chart (devtools / CI style)
//
//   Waterfall({.entries = {
//       {.label = "Read auth.ts",    .start = 0.0f, .duration = 1.2f},
//       {.label = "Edit auth.ts",    .start = 0.5f, .duration = 2.0f},
//       {.label = "Write token.ts",  .start = 1.0f, .duration = 2.3f},
//       {.label = "Edit tests",      .start = 0.8f, .duration = 1.8f,
//        .status = TaskStatus::InProgress},
//       {.label = "Run tests",       .start = 2.5f, .duration = 3.5f},
//   }})

#include "core.hpp"

namespace maya::components {

struct WaterfallEntry {
    std::string label;
    float       start    = 0.f;   // start time offset (seconds)
    float       duration = 0.f;   // duration (seconds)
    Color       color    = {};    // default: palette().primary
    TaskStatus  status   = TaskStatus::Completed;
};

struct WaterfallProps {
    std::vector<WaterfallEntry> entries = {};
    int   bar_width   = 30;      // width of the timing bars
    float time_scale  = 0.f;     // 0 = auto-scale to max end time
    bool  show_labels = true;    // show label column
    bool  show_times  = true;    // show duration text
    int   frame       = 0;       // animation frame for spinners
};

Element Waterfall(WaterfallProps props = {});

} // namespace maya::components
