#pragma once
// maya::widget::write_tool — File creation/write card
//
// Zed's new file card + Claude Code's write operation display.
//
//   ╭─ ✓ Write ────────────────────────────╮
//   │ src/utils/helpers.ts           0.1s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ + export function debounce(         │
//   │ +   fn: Function,                   │
//   │ +   delay: number                   │
//   │ + ) { ... }                         │
//   │                     12 lines written │
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

namespace maya {

enum class WriteStatus : uint8_t { Pending, Writing, Written, Failed };

class WriteTool {
    std::string file_path_;
    std::string content_;
    WriteStatus status_ = WriteStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int max_preview_lines_ = 15;

public:
    WriteTool() = default;
    explicit WriteTool(std::string path) : file_path_(std::move(path)) {}

    void set_file_path(std::string_view p) { file_path_ = std::string{p}; }
    void set_content(std::string_view c) { content_ = std::string{c}; }
    void set_status(WriteStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_preview_lines(int n) { max_preview_lines_ = n; }

    [[nodiscard]] WriteStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Write ";

        auto border_color = Color::rgb(50, 54, 62);
        if (status_ == WriteStatus::Failed)
            border_color = Color::rgb(120, 60, 65);
        else if (status_ == WriteStatus::Written)
            border_color = Color::rgb(50, 80, 55);

        auto builder = detail::box()
            .border(BorderStyle::Round)
            .border_color(border_color)
            .border_text(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            .padding(0, 1, 0, 1);

        std::vector<Element> rows;

        // File path + elapsed + line count
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

        // Expanded: show content preview
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

            auto add_prefix_style = Style{}.with_fg(Color::rgb(152, 195, 121)).with_dim();
            auto code_style = Style{}.with_fg(Color::rgb(152, 195, 121));

            std::string_view sv = content_;
            int shown = 0;
            int total_lines = 0;

            // Count total lines first
            {
                std::string_view tmp = content_;
                while (!tmp.empty()) {
                    auto nl = tmp.find('\n');
                    ++total_lines;
                    tmp = (nl == std::string_view::npos) ? std::string_view{} : tmp.substr(nl + 1);
                }
            }

            while (!sv.empty()) {
                auto nl = sv.find('\n');
                auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                if (max_preview_lines_ > 0 && shown >= max_preview_lines_) {
                    int remaining = total_lines - shown;
                    if (remaining > 0) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "  ... %d more lines", remaining);
                        rows.push_back(Element{TextElement{
                            .content = buf,
                            .style = Style{}.with_dim(),
                        }});
                    }
                    break;
                }

                std::string content = "+ ";
                content += line;
                std::vector<StyledRun> runs;
                runs.push_back(StyledRun{0, 2, add_prefix_style});
                if (!line.empty()) {
                    runs.push_back(StyledRun{2, line.size(), code_style});
                }

                rows.push_back(Element{TextElement{
                    .content = std::move(content),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = std::move(runs),
                }});
                ++shown;
            }

            // Line count summary
            {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%d lines written", total_lines);
                rows.push_back(Element{TextElement{
                    .content = buf,
                    .style = Style{}.with_dim().with_italic(),
                }});
            }
        }

        return std::move(builder)(detail::vstack()(std::move(rows)));
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case WriteStatus::Pending:
                return {"\xe2\x97\x8b", Color::rgb(92, 99, 112)};       // ○
            case WriteStatus::Writing:
                return {"\xe2\x97\x8f", Color::rgb(229, 192, 123)};     // ●
            case WriteStatus::Written:
                return {"\xe2\x9c\x93", Color::rgb(152, 195, 121)};     // ✓
            case WriteStatus::Failed:
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
