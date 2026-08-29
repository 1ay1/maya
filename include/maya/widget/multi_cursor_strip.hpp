#pragma once
// maya::widget::MultiCursorStrip — multi-cursor indicator (background-free)
//
// A compact status chip showing the current multi-cursor / selection state:
// N cursors, N selections, and the total selected length — the little readout
// editors show when you have several carets.
//
// Usage:  MultiCursorStrip{}.cursors(3).selections(2).selected_chars(48);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct MultiCursorTheme {
    Color icon  = Color::hex(0xCBA6F7);
    Color count = Color::hex(0xE6EDF3);
    Color label = Color::hex(0x7F849C);
    Color sep    = Color::hex(0x45475A);
};

struct MultiCursorStrip {
    int cursors_ = 1;
    int selections_ = 0;
    int chars_ = -1;
    MultiCursorTheme theme;

    MultiCursorStrip& cursors(int n) { cursors_ = n; return *this; }
    MultiCursorStrip& selections(int n) { selections_ = n; return *this; }
    MultiCursorStrip& selected_chars(int n) { chars_ = n; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        put("\xef\x89\x8c ", Style{}.with_fg(theme.icon)); //  columns/carets
        put(std::to_string(cursors_), Style{}.with_fg(theme.count).with_bold());
        put(cursors_ == 1 ? " cursor" : " cursors", Style{}.with_fg(theme.label));
        if (selections_ > 0) {
            put("  \xe2\x94\x82  ", Style{}.with_fg(theme.sep)); // │
            put(std::to_string(selections_), Style{}.with_fg(theme.count).with_bold());
            put(selections_ == 1 ? " selection" : " selections", Style{}.with_fg(theme.label));
        }
        if (chars_ >= 0) {
            put("  \xe2\x94\x82  ", Style{}.with_fg(theme.sep));
            put(std::to_string(chars_), Style{}.with_fg(theme.count).with_bold());
            put(" selected", Style{}.with_fg(theme.label));
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
