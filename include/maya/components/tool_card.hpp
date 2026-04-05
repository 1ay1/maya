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

Element ToolCard(ToolCardProps props = {});

} // namespace maya::components
