#pragma once
// maya::components::Timeline — Vertical event timeline (CI/pipeline style)
//
//   Timeline({.events = {
//       {.label = "Read auth middleware", .detail = "Read 42 lines",
//        .duration = "1.2s", .status = TaskStatus::Completed},
//       {.label = "Edit auth.ts", .detail = "Replaced session middleware",
//        .duration = "2.0s", .status = TaskStatus::InProgress,
//        .bar_width = 10},
//       {.label = "Run tests", .detail = "Waiting...",
//        .status = TaskStatus::Pending},
//   }})

#include "core.hpp"

namespace maya::components {

struct TimelineEvent {
    std::string label;
    std::string detail    = "";      // secondary description
    std::string duration  = "";      // e.g. "1.2s", "340ms"
    TaskStatus  status    = TaskStatus::Pending;
    int         bar_width = 0;       // optional: duration bar width (0-20), 0 = no bar
};

struct TimelineProps {
    std::vector<TimelineEvent> events = {};
    bool show_connector = true;    // vertical line between events
    bool compact        = false;   // single line per event
    int  frame          = 0;       // animation frame for spinners
    int  track_width    = 40;      // dot-track width between label and duration
};

Element Timeline(TimelineProps props = {});

} // namespace maya::components
