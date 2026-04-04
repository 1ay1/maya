#pragma once
// maya::components::ToolCard — Tool call card with status, title, collapsible body
//
//   ToolCard({.title = "Edit file: src/main.cpp",
//             .status = TaskStatus::Completed,
//             .children = { DiffView({...}) }})
//
//   ToolCard({.title = "Running: npm test",
//             .status = TaskStatus::InProgress,
//             .frame = tick,
//             .children = { CodeBlock({.code = terminal_output}) }})
//
//   ToolCard({.title = "Search: \"handleClick\"",
//             .status = TaskStatus::Completed,
//             .collapsed = true,
//             .summary = "3 results"})
//
//   // With permission prompt:
//   ToolCard({.title = "Run `rm -rf build/`",
//             .status = TaskStatus::WaitingForConfirmation,
//             .permission = perm.render(frame),
//             .tool_name = "terminal"})

#include "core.hpp"

namespace maya::components {

struct ToolCardProps {
    std::string title;
    TaskStatus      status     = TaskStatus::Pending;
    int         frame      = 0;
    bool        collapsed  = false;
    std::string summary    = "";   // shown when collapsed
    Children    children   = {};
    std::string tool_name  = "";   // e.g. "edit_file", "terminal", "grep"
    Element     permission = {};   // permission prompt (rendered by PermissionPrompt)
};

inline Element ToolCard(ToolCardProps props) {
    using namespace maya::dsl;

    auto& p = palette();
    Color sc = status_color(props.status);

    // TaskStatus icon
    std::string icon;
    if (props.status == TaskStatus::InProgress) {
        icon = spin(props.frame);
    } else if (props.status == TaskStatus::WaitingForConfirmation) {
        icon = spin(props.frame);
    } else {
        icon = status_icon(props.status);
    }

    // Header: [status icon] [tool badge] title [summary when collapsed]
    std::vector<Element> header;
    header.push_back(text(icon, Style{}.with_bold().with_fg(sc)));
    header.push_back(text(" ", Style{}));

    if (!props.tool_name.empty()) {
        header.push_back(text("[" + props.tool_name + "]",
                               Style{}.with_fg(p.muted)));
        header.push_back(text(" ", Style{}));
    }

    header.push_back(text(props.title, Style{}.with_fg(p.text)));

    if (props.collapsed && !props.summary.empty()) {
        header.push_back(text(" ", Style{}));
        header.push_back(text(props.summary, Style{}.with_fg(p.dim)));
    }

    // Expand/collapse indicator
    if (!props.children.empty()) {
        header.push_back(Element(space));
        header.push_back(text(props.collapsed ? "▸" : "▾",
                               Style{}.with_fg(p.dim)));
    }

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    // Body (when expanded)
    if (!props.collapsed) {
        for (auto& child : props.children) {
            rows.push_back(std::move(child));
        }
    }

    // Permission prompt (when waiting for confirmation)
    if (props.status == TaskStatus::WaitingForConfirmation) {
        rows.push_back(std::move(props.permission));
    }

    // TaskStatus-colored left border
    auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(sc)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
