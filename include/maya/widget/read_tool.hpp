#pragma once
// maya::widget::read_tool — File content preview card
//
// Zed's file preview card + Claude Code's line numbers and truncation.
//
//   ╭─ ✓ Read ─────────────────────────────╮
//   │ src/main.cpp                   0.2s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │   1 │ #include <iostream>             │
//   │   2 │ int main() {                    │
//   │   3 │     return 0;                   │
//   │   4 │ }                               │
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

enum class ReadStatus : uint8_t { Pending, Reading, Success, Failed };

class ReadTool {
    std::string file_path_;
    std::string content_;
    ReadStatus status_ = ReadStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int start_line_ = 1;
    int max_lines_ = 20;      // max lines to show when expanded
    int total_lines_ = 0;     // total lines in the file

public:
    ReadTool() = default;
    explicit ReadTool(std::string path) : file_path_(std::move(path)) {}

    void set_file_path(std::string_view p) { file_path_ = std::string{p}; }
    void set_content(std::string_view c) { content_ = std::string{c}; }
    void set_status(ReadStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_start_line(int n) { start_line_ = n; }
    void set_max_lines(int n) { max_lines_ = n; }
    void set_total_lines(int n) { total_lines_ = n; }

    [[nodiscard]] ReadStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Read ";

        auto border_color = Color::rgb(50, 54, 62);
        auto border_style = BorderStyle::Round;
        if (status_ == ReadStatus::Failed) {
            border_color = Color::rgb(120, 60, 65);
            border_style = BorderStyle::Dashed;
        }

        std::vector<Element> rows;

        // File path header + elapsed
        {
            std::string content = file_path_;
            std::vector<StyledRun> runs;
            auto path_style = Style{}.with_fg(Color::rgb(171, 178, 191));
            runs.push_back(StyledRun{0, file_path_.size(), path_style});

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

        // Expanded: show content with line numbers
        if (expanded_ && !content_.empty()) {
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

            auto lineno_style = Style{}.with_fg(Color::rgb(92, 99, 112));
            auto pipe_style = Style{}.with_fg(Color::rgb(50, 54, 62));
            auto code_style = Style{}.with_fg(Color::rgb(200, 204, 212));

            std::string_view sv = content_;
            int line_num = start_line_;
            int shown = 0;

            while (!sv.empty()) {
                auto nl = sv.find('\n');
                auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                if (max_lines_ > 0 && shown >= max_lines_) {
                    int remaining_lines = total_lines_ > 0
                        ? total_lines_ - (start_line_ + shown) + 1
                        : 0;
                    if (remaining_lines > 0) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "  ... %d more lines", remaining_lines);
                        rows.push_back(Element{TextElement{
                            .content = buf,
                            .style = Style{}.with_dim(),
                        }});
                    } else {
                        rows.push_back(Element{TextElement{
                            .content = "  ...",
                            .style = Style{}.with_dim(),
                        }});
                    }
                    break;
                }

                // Format: "  42 │ code here"
                char num_buf[8];
                std::snprintf(num_buf, sizeof(num_buf), "%4d", line_num);
                std::string content = std::string{"  "} + num_buf + " \xe2\x94\x82 ";
                std::vector<StyledRun> runs;

                // Line number
                runs.push_back(StyledRun{0, 2, Style{}});  // indent
                runs.push_back(StyledRun{2, 4, lineno_style});  // number
                std::size_t pipe_off = 6;
                runs.push_back(StyledRun{pipe_off, 5, pipe_style});  // " │ " (space + 3-byte UTF-8 + space)

                // Code content
                std::size_t code_off = content.size();
                content += line;
                if (!line.empty()) {
                    runs.push_back(StyledRun{code_off, line.size(), code_style});
                }

                rows.push_back(Element{TextElement{
                    .content = std::move(content),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = std::move(runs),
                }});

                ++line_num;
                ++shown;
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
            case ReadStatus::Pending:
                return {"\xe2\x97\x8b", Color::rgb(92, 99, 112)};       // ○
            case ReadStatus::Reading:
                return {"\xe2\x97\x8f", Color::rgb(229, 192, 123)};     // ●
            case ReadStatus::Success:
                return {"\xe2\x9c\x93", Color::rgb(152, 195, 121)};     // ✓
            case ReadStatus::Failed:
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
