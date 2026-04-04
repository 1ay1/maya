#pragma once
// maya::components::CodeBlock — Code display with line numbers
//
//   CodeBlock({.code = "int main() {\n    return 0;\n}"})
//   CodeBlock({.code = source, .language = "cpp", .start_line = 42})
//   CodeBlock({.code = source, .highlight_lines = {3, 5, 7}})

#include "core.hpp"
#include <set>

namespace maya::components {

struct CodeBlockProps {
    std::string code;
    std::string language       = "";
    int         start_line     = 1;
    bool        show_line_nums = true;
    Color       bg             = palette().surface;
    Color       fg             = Color::rgb(180, 200, 220);
    Color       line_num_color = palette().dim;
    Color       highlight_bg   = Color::rgb(35, 40, 55);
    std::set<int> highlight_lines = {};   // 1-based line numbers to highlight
};

inline Element CodeBlock(CodeBlockProps props) {
    using namespace maya::dsl;

    // Split code into lines
    std::vector<std::string_view> lines;
    std::string_view sv = props.code;
    while (true) {
        auto nl = sv.find('\n');
        if (nl == std::string_view::npos) {
            lines.push_back(sv);
            break;
        }
        lines.push_back(sv.substr(0, nl));
        sv = sv.substr(nl + 1);
    }

    // Calculate line number width
    int max_line = props.start_line + static_cast<int>(lines.size()) - 1;
    int num_width = 1;
    { int n = max_line; while (n >= 10) { n /= 10; ++num_width; } }

    std::vector<Element> rows;

    // Language label
    if (!props.language.empty()) {
        rows.push_back(text(" " + props.language,
                            Style{}.with_dim().with_fg(palette().muted)));
    }

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        int line_no = props.start_line + i;
        bool highlighted = props.highlight_lines.contains(line_no);

        Style code_style = Style{}.with_fg(props.fg);
        if (highlighted) code_style = code_style.with_bg(props.highlight_bg);

        std::vector<Element> cols;

        // Line number
        if (props.show_line_nums) {
            auto num = std::to_string(line_no);
            while (static_cast<int>(num.size()) < num_width) num = " " + num;
            num += " │ ";
            Style num_style = Style{}.with_fg(props.line_num_color);
            if (highlighted) num_style = num_style.with_bg(props.highlight_bg);
            cols.push_back(text(std::move(num), num_style));
        }

        cols.push_back(text(std::string(lines[i]), code_style));

        rows.push_back(hstack()(std::move(cols)));
    }

    return vstack().bg(props.bg).padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
