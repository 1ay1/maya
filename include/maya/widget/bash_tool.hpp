#pragma once
// maya::widget::bash_tool — Command execution card
//
// Zed's bordered terminal card + Claude Code's command status UX.
//
//   ╭─ ● Execute ──────────────────────────╮
//   │ $ npm install --save-dev typescript   │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ added 1 package in 2.3s              │
//   ╰──────────────────────────────────────╯
//
// Collapsed:
//   ╭─ ✓ Execute ──────────────────────────╮
//   │ $ npm install --save-dev typescript   │
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

enum class BashStatus : uint8_t { Pending, Running, Success, Failed };

class BashTool {
    std::string command_;
    std::string output_;
    BashStatus status_ = BashStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int exit_code_ = 0;
    int max_output_lines_ = 0;  // 0 = unlimited

public:
    BashTool() = default;
    explicit BashTool(std::string command) : command_(std::move(command)) {}

    void set_command(std::string_view cmd) { command_ = std::string{cmd}; }
    void set_output(std::string_view out) { output_ = std::string{out}; }
    void append_output(std::string_view text) { output_ += text; }
    void set_status(BashStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_exit_code(int code) { exit_code_ = code; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_output_lines(int n) { max_output_lines_ = n; }

    [[nodiscard]] BashStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Status icon for border label
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Execute ";

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == BashStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        }

        std::vector<Element> rows;

        // Command line: "$ command"
        {
            std::string content = "$ " + command_;
            std::vector<StyledRun> runs;
            auto prompt_style = Style{}.with_bold().with_fg(Color::green());
            runs.push_back(StyledRun{0, 2, prompt_style});  // "$ "

            // Elapsed time at end
            if (elapsed_ > 0.0f) {
                std::string ts = "  " + format_elapsed();
                auto time_style = Style{}.with_dim();
                runs.push_back(StyledRun{content.size(), 2, Style{}});
                runs.push_back(StyledRun{content.size() + 2, ts.size() - 2, time_style});
                content += ts;
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Exit code badge if failed
        if (status_ == BashStatus::Failed && exit_code_ != 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "exit code %d", exit_code_);
            rows.push_back(Element{TextElement{
                .content = buf,
                .style = Style{}.with_fg(Color::red()).with_dim(),
            }});
        }

        // Output (expanded only)
        if (expanded_ && !output_.empty()) {
            // Dashed separator
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim(),
                    }};
                },
                .layout = {},
            }});

            // Output lines — terminal default fg
            auto output_style = Style{};
            std::string_view sv = output_;
            int line_count = 0;

            while (!sv.empty()) {
                auto nl = sv.find('\n');
                auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                if (max_output_lines_ > 0 && line_count >= max_output_lines_) {
                    char buf[32];
                    int remaining = 0;
                    auto tmp = sv;
                    while (!tmp.empty()) {
                        auto p = tmp.find('\n');
                        ++remaining;
                        tmp = (p == std::string_view::npos) ? std::string_view{} : tmp.substr(p + 1);
                    }
                    std::snprintf(buf, sizeof(buf), "... %d more lines", remaining + 1);
                    rows.push_back(Element{TextElement{
                        .content = buf,
                        .style = Style{}.with_dim(),
                    }});
                    break;
                }

                rows.push_back(Element{TextElement{
                    .content = std::string{line},
                    .style = output_style,
                    .wrap = TextWrap::Wrap,
                }});
                ++line_count;
            }
        }

        return (dsl::v(std::move(rows))
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case BashStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case BashStatus::Running:
                return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case BashStatus::Success:
                return {"\xe2\x9c\x93", Color::green()};          // ✓
            case BashStatus::Failed:
                return {"\xe2\x9c\x97", Color::red()};            // ✗
        }
        return {"\xe2\x97\x8b", Color::bright_black()};
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
