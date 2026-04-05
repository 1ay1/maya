#pragma once
// maya::components::ActivityBar — Sidebar with plan/edits/status sections
//
// Mirrors Zed's activity bar: shows current plan progress, file edits,
// pending items, and subagent status in collapsible sections.
//
//   ActivityBar bar({
//       .plan_items = {{.text = "Analyze codebase", .status = TaskStatus::Completed}, ...},
//       .edits = {{.path = "src/main.cpp", .added = 12, .removed = 3}, ...},
//   });
//   bar.render()

#include "core.hpp"
#include "disclosure.hpp"
#include "diff_stat.hpp"

namespace maya::components {

struct PlanItem {
    std::string text;
    TaskStatus      status = TaskStatus::Pending;
};

struct FileEdit {
    std::string path;
    int added   = 0;
    int removed = 0;
};

struct ActivityBarProps {
    std::vector<PlanItem> plan_items = {};
    std::vector<FileEdit> edits      = {};
    int  pending_messages = 0;
    bool show_plan   = true;
    bool show_edits  = true;
};

class ActivityBar {
    Disclosure plan_disc_;
    Disclosure edits_disc_;
    ActivityBarProps props_;

public:
    explicit ActivityBar(ActivityBarProps props = {})
        : plan_disc_(DisclosureProps{.title = "Plan", .expanded = true})
        , edits_disc_(DisclosureProps{.title = "Edits", .expanded = true})
        , props_(std::move(props)) {}

    void set_plan(std::vector<PlanItem> items) { props_.plan_items = std::move(items); }
    void set_edits(std::vector<FileEdit> edits) { props_.edits = std::move(edits); }
    void set_pending(int n) { props_.pending_messages = n; }

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
