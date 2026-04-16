#pragma once
// maya::widget::tool_call — Collapsible tool execution card
//
// Best of Zed + Claude Code: Zed's bordered card presentation with
// Claude Code's status indicators, elapsed time, and expand/collapse.
//
//   ╭─ ✓ Read ─────────────────────────────╮
//   │ src/main.cpp                    0.3s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ <expanded content>                    │
//   ╰──────────────────────────────────────╯
//
// Collapsed:
//   ╭─ ✓ Read ─────────────────────────────╮
//   │ src/main.cpp                    0.3s  │
//   ╰──────────────────────────────────────╯

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ToolCallStatus : uint8_t {
    Pending,
    Running,
    Completed,
    Failed,
    Confirmation,
};

enum class ToolCallKind : uint8_t {
    Read, Edit, Execute, Search, Delete,
    Move, Fetch, Think, Agent, Other,
};

class ToolCall {
public:
    struct Config {
        Config() = default;
        std::string tool_name;
        ToolCallKind kind = ToolCallKind::Other;
        std::string description;
    };

private:
    Config cfg_;
    ToolCallStatus status_ = ToolCallStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    std::optional<Element> content_;

public:
    explicit ToolCall(Config cfg) : cfg_(std::move(cfg)) {}

    void set_status(ToolCallStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_content(Element content) { content_ = std::move(content); }

    [[nodiscard]] ToolCallStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Border label: " ✓ Read " or " ● Execute "
        auto [icon, icon_style] = status_icon();
        std::string border_label = " " + icon + " " + cfg_.tool_name + " ";

        auto border_color = Color::rgb(50, 54, 62);
        auto border_style = BorderStyle::Round;
        // Tint + style border based on status
        if (status_ == ToolCallStatus::Failed) {
            border_color = Color::rgb(120, 60, 65);
            border_style = BorderStyle::Dashed;
        } else if (status_ == ToolCallStatus::Confirmation) {
            border_color = Color::rgb(120, 100, 50);
        }

        if (expanded_ && content_) {
            return (dsl::v(build_header(), *content_)
                | dsl::border(border_style)
                | dsl::bcolor(border_color)
                | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
                | dsl::padding(0, 1, 0, 1)).build();
        }

        return (dsl::v(build_header())
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    [[nodiscard]] Element build_header() const {
        std::string content;
        std::vector<StyledRun> runs;

        // Description
        if (!cfg_.description.empty()) {
            auto style = Style{}.with_fg(Color::rgb(171, 178, 191));
            runs.push_back(StyledRun{content.size(), cfg_.description.size(), style});
            content += cfg_.description;
        }

        // Elapsed (right-aligned feel via spacing)
        if (elapsed_ > 0.0f) {
            if (!content.empty()) {
                runs.push_back(StyledRun{content.size(), 2, Style{}});
                content += "  ";
            }
            auto time_style = Style{}.with_dim();
            std::string ts = format_elapsed();
            runs.push_back(StyledRun{content.size(), ts.size(), time_style});
            content += ts;
        }

        if (content.empty()) return Element{TextElement{}};

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    struct IconAndStyle { std::string icon; Style style; };

    [[nodiscard]] IconAndStyle status_icon() const {
        switch (status_) {
            case ToolCallStatus::Pending:
                return {"\xe2\x97\x8b", Style{}.with_dim()};                          // ○
            case ToolCallStatus::Running:
                return {"\xe2\x97\x8f", Style{}.with_fg(Color::rgb(229, 192, 123))};  // ●
            case ToolCallStatus::Completed:
                return {"\xe2\x9c\x93", Style{}.with_fg(Color::rgb(152, 195, 121))};  // ✓
            case ToolCallStatus::Failed:
                return {"\xe2\x9c\x97", Style{}.with_fg(Color::rgb(224, 108, 117))};  // ✗
            case ToolCallStatus::Confirmation:
                return {"\xe2\x9a\xa0", Style{}.with_fg(Color::rgb(229, 192, 123))};  // ⚠
        }
        return {"\xe2\x97\x8b", Style{}.with_dim()};
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
