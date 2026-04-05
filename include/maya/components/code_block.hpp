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

Element CodeBlock(CodeBlockProps props = {});

} // namespace maya::components
