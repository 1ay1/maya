#pragma once
// maya::widget::agent_tool — Sub-agent card with nested status
//
// Zed's bordered nested card + Claude Code's agent spawning UX.
//
//   ╭─ ● Agent ────────────────────────────╮
//   │ Exploring codebase structure    ⠋    │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │   ╭─ ✓ Read ────────────────╮        │
//   │   │ src/main.cpp      0.2s  │        │
//   │   ╰─────────────────────────╯        │
//   │   ╭─ ✓ Grep ────────────────╮        │
//   │   │ "TODO" — 3 matches      │        │
//   │   ╰─────────────────────────╯        │
//   ╰──────────────────────────────────────╯

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "spinner.hpp"

namespace maya {

enum class AgentStatus : uint8_t { Pending, Running, Completed, Failed };

class AgentTool {
    std::string description_;
    std::string model_;
    AgentStatus status_ = AgentStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = true;
    std::vector<Element> nested_tools_;
    Spinner<SpinnerStyle::Dots> spinner_{Style{}.with_dim()};

public:
    AgentTool() = default;
    explicit AgentTool(std::string desc) : description_(std::move(desc)) {}

    void set_description(std::string_view d) { description_ = std::string{d}; }
    void set_model(std::string_view m) { model_ = std::string{m}; }
    void set_status(AgentStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }

    void advance(float dt) { spinner_.advance(dt); }

    void clear_tools() { nested_tools_.clear(); }
    void add_tool(Element tool) { nested_tools_.push_back(std::move(tool)); }

    [[nodiscard]] AgentStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Agent ";

        auto border_color = Color::rgb(60, 55, 80);  // subtle purple tint for agents
        if (status_ == AgentStatus::Failed)
            border_color = Color::rgb(120, 60, 65);
        else if (status_ == AgentStatus::Running)
            border_color = Color::rgb(75, 65, 100);

        auto builder = detail::box()
            .border(BorderStyle::Round)
            .border_color(border_color)
            .border_text(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            .padding(0, 1, 0, 1);

        std::vector<Element> rows;

        // Description header + model + spinner/elapsed
        {
            auto desc_style = Style{}.with_fg(Color::rgb(200, 204, 212));

            if (status_ == AgentStatus::Running) {
                // Active: description + spinner
                std::vector<Element> header_parts;
                header_parts.push_back(Element{TextElement{
                    .content = description_,
                    .style = desc_style,
                    .wrap = TextWrap::NoWrap,
                }});
                header_parts.push_back(Element{TextElement{
                    .content = "  ",
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                }});
                header_parts.push_back(spinner_.build());
                rows.push_back(detail::hstack()(std::move(header_parts)));
            } else {
                std::string content = description_;
                std::vector<StyledRun> runs;
                runs.push_back(StyledRun{0, description_.size(), desc_style});

                if (elapsed_ > 0.0f) {
                    std::string ts = "  " + format_elapsed();
                    runs.push_back(StyledRun{content.size(), 2, Style{}});
                    runs.push_back(StyledRun{content.size() + 2, ts.size() - 2, Style{}.with_dim()});
                    content += ts;
                }

                rows.push_back(Element{TextElement{
                    .content = std::move(content),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = std::move(runs),
                }});
            }
        }

        // Model badge (if set)
        if (!model_.empty()) {
            auto model_style = Style{}.with_dim().with_italic();
            rows.push_back(Element{TextElement{
                .content = "model: " + model_,
                .style = model_style,
            }});
        }

        // Expanded: show nested tool calls
        if (expanded_ && !nested_tools_.empty()) {
            // Separator
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim().with_fg(Color::rgb(50, 54, 62)),
                    }};
                },
                .layout = {},
            }});

            for (auto const& tool : nested_tools_) {
                rows.push_back(tool);
            }
        }

        return std::move(builder)(detail::vstack()(std::move(rows)));
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case AgentStatus::Pending:
                return {"\xe2\x97\x8b", Color::rgb(92, 99, 112)};       // ○
            case AgentStatus::Running:
                return {"\xe2\x97\x8f", Color::rgb(198, 160, 246)};     // ● purple
            case AgentStatus::Completed:
                return {"\xe2\x9c\x93", Color::rgb(152, 195, 121)};     // ✓
            case AgentStatus::Failed:
                return {"\xe2\x9c\x97", Color::rgb(224, 108, 117)};     // ✗
        }
        return {"\xe2\x97\x8b", Color::rgb(92, 99, 112)};
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
