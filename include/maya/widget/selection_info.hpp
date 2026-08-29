#pragma once
// maya::widget::SelectionInfo — cursor / selection readout (background-free)
//
// The little status readout of the cursor position and current selection:
// "Ln 42, Col 7" and, when there's a selection, "(3 lines, 128 selected)".
//
// Usage:  SelectionInfo{}.at(42, 7).selection(3, 128);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SelectionInfoTheme {
    Color num   = Color::hex(0xE6EDF3);
    Color label = Color::hex(0x7F849C);
    Color sel    = Color::hex(0xF9E2AF);
};

struct SelectionInfo {
    int line_ = 1, col_ = 1;
    int sel_lines_ = 0, sel_chars_ = 0;
    SelectionInfoTheme theme;

    SelectionInfo& at(int line, int col) { line_ = line; col_ = col; return *this; }
    SelectionInfo& selection(int lines, int chars) { sel_lines_ = lines; sel_chars_ = chars; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        put("Ln ", Style{}.with_fg(theme.label));
        put(std::to_string(line_), Style{}.with_fg(theme.num).with_bold());
        put(", Col ", Style{}.with_fg(theme.label));
        put(std::to_string(col_), Style{}.with_fg(theme.num).with_bold());
        if (sel_chars_ > 0) {
            put("   (", Style{}.with_fg(theme.label));
            if (sel_lines_ > 1) {
                put(std::to_string(sel_lines_), Style{}.with_fg(theme.sel).with_bold());
                put(" lines, ", Style{}.with_fg(theme.label));
            }
            put(std::to_string(sel_chars_), Style{}.with_fg(theme.sel).with_bold());
            put(" selected)", Style{}.with_fg(theme.label));
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
