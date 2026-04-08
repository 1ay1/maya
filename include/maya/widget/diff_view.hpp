#pragma once
// maya::widget::diff_view — Unified diff display with syntax coloring
//
// Renders unified diff output with +/- line coloring, file headers, and
// hunk markers. Designed for AI coding assistants showing file edits.
//
// Usage:
//   DiffView diff("src/main.cpp", R"(
//   @@ -12,3 +12,4 @@
//    int main() {
//   -    return 0;
//   +    auto result = run();
//   +    return result;
//    }
//   )");
//   auto ui = diff.build();

#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

struct DiffViewConfig {
    Style add_style      = Style{}.with_fg(Color::rgb(80, 220, 120));
    Style remove_style   = Style{}.with_fg(Color::rgb(255, 80, 80));
    Style context_style  = Style{}.with_dim();
    Style hunk_style     = Style{}.with_fg(Color::rgb(100, 180, 255)).with_dim();
    Style header_style   = Style{}.with_bold().with_fg(Color::rgb(220, 220, 240));
    Style line_no_style  = Style{}.with_dim();
    Color border_color   = Color::rgb(60, 60, 80);
    bool show_border     = true;
    bool show_line_numbers = false;
};

class DiffView {
    std::string filename_;
    std::string diff_;
    DiffViewConfig cfg_;

public:
    DiffView() = default;

    explicit DiffView(std::string_view filename, std::string_view diff,
                      DiffViewConfig cfg = {})
        : filename_(filename), diff_(diff), cfg_(std::move(cfg)) {}

    void set_diff(std::string_view filename, std::string_view diff) {
        filename_ = std::string{filename};
        diff_ = std::string{diff};
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;

        // Parse diff lines
        std::string_view remaining{diff_};
        int old_line = 0, new_line = 0;

        while (!remaining.empty()) {
            auto nl = remaining.find('\n');
            std::string_view line;
            if (nl == std::string_view::npos) {
                line = remaining;
                remaining = {};
            } else {
                line = remaining.substr(0, nl);
                remaining = remaining.substr(nl + 1);
            }

            if (line.empty()) continue;

            if (line.starts_with("@@")) {
                // Parse hunk header for line numbers
                parse_hunk(line, old_line, new_line);
                rows.push_back(Element{TextElement{
                    .content = std::string{line}, .style = cfg_.hunk_style}});
            } else if (line.starts_with('+')) {
                auto row = build_diff_line(line, cfg_.add_style, new_line);
                rows.push_back(std::move(row));
                ++new_line;
            } else if (line.starts_with('-')) {
                auto row = build_diff_line(line, cfg_.remove_style, old_line);
                rows.push_back(std::move(row));
                ++old_line;
            } else {
                // Context line (starts with ' ' or bare text)
                rows.push_back(build_diff_line(line, cfg_.context_style, old_line));
                ++old_line;
                ++new_line;
            }
        }

        auto body = detail::vstack()(std::move(rows));

        if (!cfg_.show_border) return body;

        // Wrap in a bordered box with filename header
        std::string title = filename_.empty() ? "diff" : filename_;
        return detail::vstack()
            .border(BorderStyle::Round)
            .border_color(cfg_.border_color)
            .border_text(title, BorderTextPos::Top)(
                std::move(body));
    }

private:
    [[nodiscard]] Element build_diff_line(std::string_view line, const Style& s,
                                           int lineno) const {
        if (cfg_.show_line_numbers) {
            std::string num = std::to_string(lineno);
            while (num.size() < 4) num = " " + num;
            num += " ";
            return detail::hstack()(
                Element{TextElement{.content = std::move(num), .style = cfg_.line_no_style}},
                Element{TextElement{.content = std::string{line}, .style = s}});
        }
        return Element{TextElement{.content = std::string{line}, .style = s}};
    }

    static void parse_hunk(std::string_view hunk, int& old_line, int& new_line) {
        // Parse "@@ -old,count +new,count @@"
        auto pos = hunk.find('-');
        if (pos == std::string_view::npos) return;
        auto comma = hunk.find(',', pos);
        auto plus = hunk.find('+', pos);
        if (plus == std::string_view::npos) return;

        auto parse_int = [](std::string_view s) -> int {
            int val = 0;
            for (char c : s) {
                if (c >= '0' && c <= '9') val = val * 10 + (c - '0');
                else if (val > 0) break;
            }
            return val;
        };

        old_line = parse_int(hunk.substr(pos + 1));
        new_line = parse_int(hunk.substr(plus + 1));
    }
};

} // namespace maya
