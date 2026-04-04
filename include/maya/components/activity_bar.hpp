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

    [[nodiscard]] Element render(int frame = 0) const {
        using namespace maya::dsl;
        auto& p = palette();

        std::vector<Element> sections;

        // ── Plan section ─────────────────────────────────────────────────────
        if (props_.show_plan && !props_.plan_items.empty()) {
            int completed = 0;
            for (auto& item : props_.plan_items)
                if (item.status == TaskStatus::Completed) ++completed;

            auto badge = std::to_string(completed) + "/" +
                         std::to_string(props_.plan_items.size());

            Children plan_children;
            for (auto& item : props_.plan_items) {
                Color ic = status_color(item.status);
                std::string icon;
                if (item.status == TaskStatus::InProgress)
                    icon = std::string(spin(frame));
                else
                    icon = status_icon(item.status);

                Style ts = Style{}.with_fg(
                    item.status == TaskStatus::Completed ? p.dim : p.text);
                if (item.status == TaskStatus::Completed) ts = ts.with_strikethrough();

                plan_children.push_back(
                    hstack().gap(1)(
                        text(icon, Style{}.with_fg(ic)),
                        text(item.text, ts)
                    )
                );
            }

            auto disc = Disclosure(DisclosureProps{
                .title = "Plan", .expanded = plan_disc_.expanded(),
                .badge = badge});
            sections.push_back(disc.render(std::move(plan_children)));
        }

        // ── Edits section ────────────────────────────────────────────────────
        if (props_.show_edits && !props_.edits.empty()) {
            Children edit_children;
            int total_add = 0, total_del = 0;
            for (auto& e : props_.edits) {
                total_add += e.added;
                total_del += e.removed;

                edit_children.push_back(
                    hstack()(
                        text(e.path, Style{}.with_fg(p.text)),
                        Element(space),
                        DiffStat({.added = e.added, .removed = e.removed})
                    )
                );
            }

            auto badge = "+" + std::to_string(total_add) + " / -" + std::to_string(total_del);
            auto disc = Disclosure(DisclosureProps{
                .title = "Edits", .expanded = edits_disc_.expanded(),
                .badge = badge});
            sections.push_back(disc.render(std::move(edit_children)));
        }

        // ── Pending messages ─────────────────────────────────────────────────
        if (props_.pending_messages > 0) {
            sections.push_back(
                hstack().gap(1)(
                    text("◔", Style{}.with_fg(p.warning)),
                    text(std::to_string(props_.pending_messages) + " pending",
                         Style{}.with_fg(p.warning))
                )
            );
        }

        if (sections.empty()) {
            return text("  No activity", Style{}.with_italic().with_fg(p.dim));
        }

        return vstack().gap(1)(std::move(sections));
    }
};

} // namespace maya::components
