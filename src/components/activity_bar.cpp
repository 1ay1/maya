#include "maya/components/activity_bar.hpp"

namespace maya::components {

Element ActivityBar::render(int frame) const {
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

} // namespace maya::components
