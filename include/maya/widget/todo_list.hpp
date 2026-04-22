#pragma once
// maya::widget::todo_list — Session todo list card
//
//   ╭─ ● Todo · 2 of 5 ────────────────────╮
//   │ ✓  Write the widget                   │
//   │ ✓  Wire it into the thread            │
//   │ ●  Add tests                           │
//   │ ○  Document it                         │
//   │ ○  Ship it                             │
//   ╰──────────────────────────────────────╯

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class TodoItemStatus : uint8_t { Pending, InProgress, Completed };
enum class TodoListStatus : uint8_t { Pending, Running, Done, Failed };

struct TodoListItem {
    std::string content;
    TodoItemStatus status = TodoItemStatus::Pending;
};

class TodoListTool {
    std::vector<TodoListItem> items_;
    std::string description_;
    TodoListStatus status_ = TodoListStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = true;

public:
    TodoListTool() = default;

    void add(TodoListItem i) { items_.push_back(std::move(i)); }
    void set_items(std::vector<TodoListItem> v) { items_ = std::move(v); }
    void set_description(std::string_view d) { description_ = std::string{d}; }
    void set_status(TodoListStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }

    [[nodiscard]] TodoListStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    [[nodiscard]] int completed_count() const {
        int n = 0;
        for (const auto& i : items_)
            if (i.status == TodoItemStatus::Completed) ++n;
        return n;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, _icon_color] = status_icon();
        // Progress in the title bar so it reads at a glance even when collapsed.
        std::string border_label = " " + icon + " Todo";
        if (!items_.empty()) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), " \xc2\xb7 %d of %zu",
                completed_count(), items_.size());
            border_label += buf;
        }
        border_label += " ";

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == TodoListStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        } else if (status_ == TodoListStatus::Done && all_completed()) {
            border_color = Color::green();
        }

        std::vector<Element> rows;

        // Optional description / elapsed header row. Matches Write/Edit layout:
        // dim text on the left, elapsed on the right.
        if (!description_.empty() || elapsed_ > 0.0f) {
            std::string content;
            std::vector<StyledRun> runs;
            if (!description_.empty()) {
                runs.push_back(StyledRun{0, description_.size(), Style{}.with_dim()});
                content = description_;
            }
            if (elapsed_ > 0.0f) {
                std::string ts = content.empty() ? format_elapsed()
                                                 : std::string{"  "} + format_elapsed();
                std::size_t off = content.size();
                runs.push_back(StyledRun{off, ts.size(), Style{}.with_dim()});
                content += ts;
            }
            if (!content.empty()) {
                rows.push_back(Element{TextElement{
                    .content = std::move(content),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = std::move(runs),
                }});
            }
        }

        if (expanded_) {
            for (const auto& item : items_)
                rows.push_back(render_item(item));
        }

        return (dsl::v(std::move(rows))
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    [[nodiscard]] bool all_completed() const {
        if (items_.empty()) return false;
        for (const auto& i : items_)
            if (i.status != TodoItemStatus::Completed) return false;
        return true;
    }

    [[nodiscard]] bool any_in_progress() const {
        for (const auto& i : items_)
            if (i.status == TodoItemStatus::InProgress) return true;
        return false;
    }

    [[nodiscard]] Element render_item(const TodoListItem& item) const {
        auto [mark, mark_style] = item_mark(item.status);
        Style text_style = item_text_style(item.status);
        std::string content;
        content += mark;
        content += "  ";
        std::size_t text_off = content.size();
        content += item.content;
        std::vector<StyledRun> runs;
        runs.push_back(StyledRun{0, mark.size(), mark_style});
        runs.push_back(StyledRun{text_off, item.content.size(), text_style});
        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::Wrap,
            .runs = std::move(runs),
        }};
    }

    struct MarkInfo { std::string mark; Style style; };

    [[nodiscard]] MarkInfo item_mark(TodoItemStatus s) const {
        switch (s) {
            case TodoItemStatus::Completed:
                return {"\xe2\x9c\x93", Style{}.with_fg(Color::green())};   // ✓
            case TodoItemStatus::InProgress:
                return {"\xe2\x97\x8f", Style{}.with_fg(Color::yellow())};  // ●
            case TodoItemStatus::Pending:
                return {"\xe2\x97\x8b", Style{}.with_dim()};                // ○
        }
        return {"\xe2\x97\x8b", Style{}.with_dim()};
    }

    [[nodiscard]] Style item_text_style(TodoItemStatus s) const {
        switch (s) {
            case TodoItemStatus::Completed:
                return Style{}.with_dim().with_strikethrough();
            case TodoItemStatus::InProgress:
                return Style{}.with_bold();
            case TodoItemStatus::Pending:
                return Style{};
        }
        return Style{};
    }

    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        if (status_ == TodoListStatus::Failed)
            return {"\xe2\x9c\x97", Color::red()};                         // ✗
        if (!items_.empty() && all_completed())
            return {"\xe2\x9c\x93", Color::green()};                       // ✓
        if (any_in_progress() || status_ == TodoListStatus::Running)
            return {"\xe2\x97\x8f", Color::yellow()};                      // ●
        return {"\xe2\x97\x8b", Color::bright_black()};                    // ○
    }

    [[nodiscard]] std::string format_elapsed() const {
        char buf[32];
        if (elapsed_ < 1.0f) {
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(elapsed_ * 1000.0f));
            return buf;
        }
        if (elapsed_ < 60.0f) {
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(elapsed_));
            return buf;
        }
        int mins = static_cast<int>(elapsed_) / 60;
        float secs = elapsed_ - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(secs));
        return buf;
    }
};

} // namespace maya
