#pragma once
// maya::widget::plan_view — Task/plan display with status icons
//
// Displays a list of tasks with status indicators matching Zed's plan entries.
//
// Usage:
//   PlanView plan;
//   plan.add("Analyze codebase", TaskStatus::InProgress);
//   plan.add("Read configuration files", TaskStatus::Completed);
//   plan.add("Write implementation");
//   auto ui = plan.build();

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

enum class TaskStatus : uint8_t { Pending, InProgress, Completed };

struct PlanView {
    struct Task {
        std::string label;
        TaskStatus status = TaskStatus::Pending;
    };

    std::vector<Task> tasks;

    void add(std::string label, TaskStatus status = TaskStatus::Pending) {
        tasks.push_back({std::move(label), status});
    }

    void set_status(size_t index, TaskStatus status) {
        if (index < tasks.size()) tasks[index].status = status;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Colors
        constexpr auto pending_color    = Color::rgb(92, 99, 112);
        constexpr auto inprogress_color = Color::rgb(97, 175, 239);
        constexpr auto completed_color  = Color::rgb(152, 195, 121);

        std::vector<Element> rows;
        rows.reserve(tasks.size());

        for (auto const& task : tasks) {
            std::string content;
            std::vector<StyledRun> runs;

            // 2-space indent
            std::string indent = "  ";
            content += indent;

            std::string icon;
            Style icon_style;
            Style text_style;

            switch (task.status) {
            case TaskStatus::Pending:
                icon = "○";
                icon_style = Style{}.with_fg(pending_color);
                text_style = Style{}.with_fg(pending_color);
                break;
            case TaskStatus::InProgress:
                icon = "●";
                icon_style = Style{}.with_fg(inprogress_color);
                text_style = Style{}.with_fg(inprogress_color);
                break;
            case TaskStatus::Completed:
                icon = "✓";
                icon_style = Style{}.with_fg(completed_color);
                text_style = Style{}.with_dim().with_strikethrough().with_fg(completed_color);
                break;
            }

            // Indent run (no style)
            runs.push_back({0, indent.size(), Style{}});

            // Icon run
            std::size_t icon_offset = content.size();
            content += icon;
            runs.push_back({icon_offset, icon.size(), icon_style});

            // Space after icon
            std::size_t space_offset = content.size();
            content += " ";
            runs.push_back({space_offset, 1, Style{}});

            // Label run
            std::size_t label_offset = content.size();
            content += task.label;
            runs.push_back({label_offset, task.label.size(), text_style});

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return detail::vstack()(std::move(rows));
    }
};

} // namespace maya
