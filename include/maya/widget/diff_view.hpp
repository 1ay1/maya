#pragma once
// maya::widget::diff_view — Unified diff display for file changes
//
// Renders unified diffs with colored +/- lines matching Zed's agent diff style.
// Shows file path header, hunk markers, and colored added/removed/context lines.
//
// Usage:
//   auto diff = DiffView("src/main.cpp",
//       "@@ -12,4 +12,6 @@\n"
//       " void Canvas::begin_frame() {\n"
//       "-    damage_ = {0, 0, width_, height_};\n"
//       "+    damage_ = {0, 0, 0, 0};\n"
//       "+    // Start with empty damage rect\n"
//       " }\n");
//   auto elem = diff.build();

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct DiffView {
    struct Config {
        Style add_style    = Style{}.with_fg(Color::green());
        Style remove_style = Style{}.with_fg(Color::red());
        Style context_style= Style{}.with_dim();
        Style hunk_style   = Style{}.with_fg(Color::blue()).with_dim();
        Style header_style = Style{}.with_fg(Color::blue()).with_bold();
        Style lineno_style = Style{}.with_dim();
        Color border_color = Color::bright_black();
        bool show_border = true;
        bool show_line_numbers = true;
    };

    std::string file_path;
    std::string diff_text;
    Config config;

    DiffView(std::string path, std::string diff)
        : file_path(std::move(path)), diff_text(std::move(diff)) {}

    DiffView(std::string path, std::string diff, Config cfg)
        : file_path(std::move(path)), diff_text(std::move(diff)), config(std::move(cfg)) {}

    [[nodiscard]] Element build() const {
        std::vector<Element> lines;
        lines.reserve(32);

        // Parse diff lines
        std::string_view sv = diff_text;
        int old_line = 0, new_line = 0;

        while (!sv.empty()) {
            auto nl = sv.find('\n');
            auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
            sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

            if (line.empty()) continue;

            if (line.starts_with("@@")) {
                // Parse hunk header for line numbers
                auto plus_pos = line.find('+');
                if (plus_pos != std::string_view::npos) {
                    auto comma = line.find(',', plus_pos);
                    auto end = line.find(' ', plus_pos);
                    if (end == std::string_view::npos) end = line.size();
                    auto num_str = line.substr(plus_pos + 1,
                        (comma != std::string_view::npos && comma < end ? comma : end) - plus_pos - 1);
                    new_line = 0;
                    for (char c : num_str) {
                        if (c >= '0' && c <= '9') new_line = new_line * 10 + (c - '0');
                    }
                    old_line = new_line;
                }
                lines.push_back(Element{TextElement{
                    .content = std::string(line),
                    .style = config.hunk_style,
                }});
                continue;
            }

            // Build line with optional line number
            std::string content;
            std::vector<StyledRun> runs;
            Style line_style;

            if (line[0] == '+') {
                line_style = config.add_style;
                if (config.show_line_numbers) {
                    auto num = std::to_string(new_line++);
                    while (num.size() < 4) num = " " + num;
                    num += " ";
                    runs.push_back(StyledRun{0, num.size(), config.lineno_style});
                    content += num;
                }
                content += "+";
                content += line.substr(1);
                runs.push_back(StyledRun{content.size() - line.size(), line.size(), config.add_style});
            } else if (line[0] == '-') {
                line_style = config.remove_style;
                if (config.show_line_numbers) {
                    std::string num = "     ";  // blank for removed
                    runs.push_back(StyledRun{0, num.size(), config.lineno_style});
                    content += num;
                }
                content += "-";
                content += line.substr(1);
                runs.push_back(StyledRun{content.size() - line.size(), line.size(), config.remove_style});
            } else {
                line_style = config.context_style;
                if (config.show_line_numbers) {
                    auto num = std::to_string(new_line++);
                    while (num.size() < 4) num = " " + num;
                    num += " ";
                    runs.push_back(StyledRun{0, num.size(), config.lineno_style});
                    content += num;
                }
                if (line[0] == ' ') {
                    content += " ";
                    content += line.substr(1);
                } else {
                    content += line;
                }
                runs.push_back(StyledRun{content.size() - (line[0] == ' ' ? line.size() : line.size()),
                                          line.size(), config.context_style});
                ++old_line;
            }

            lines.push_back(Element{TextElement{
                .content = std::move(content),
                .style = line_style,
                .runs = std::move(runs),
            }});
        }

        if (config.show_border) {
            return (dsl::v(std::move(lines))
                | dsl::border(BorderStyle::Round)
                | dsl::bcolor(config.border_color)
                | dsl::btext(" " + file_path + " ", BorderTextPos::Top)
                | dsl::padding(0, 1, 0, 1)).build();
        }

        return dsl::v(std::move(lines)).build();
    }
};

} // namespace maya
