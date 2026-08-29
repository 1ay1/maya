#pragma once
// maya::widget::TreeFilterBar — filter input for a tree/list (background-free)
//
// The compact filter row above a file tree or list: a search glyph, the query
// (or placeholder), a match count, and small case / fuzzy toggle chips that
// light up when active.
//
// Usage:
//   TreeFilterBar f; f.query = "rope"; f.matches = 3; f.total = 40;
//   f.case_sensitive = false; f.fuzzy = true;
//   Element ui = f;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct TreeFilterBarTheme {
    Color icon   = Color::hex(0x585B70);
    Color query  = Color::hex(0xCDD6F4);
    Color prompt  = Color::hex(0x585B70);
    Color count    = Color::hex(0xBAC2DE);
    Color no_match  = Color::hex(0xF38BA8);
    Color on         = Color::hex(0x89B4FA);
    Color off         = Color::hex(0x45475A);
};

struct TreeFilterBar {
    std::string query;
    int         matches = 0, total = 0;
    bool        case_sensitive = false, fuzzy = false;
    TreeFilterBarTheme theme;

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        auto toggle=[&](std::string_view lbl, bool on){
            Style st = Style{}.with_fg(on ? theme.on : theme.off);
            if (on) st = st.with_bold();
            put(lbl, st); put(" ", Style{});
        };
        put("\xef\x80\x82  ", Style{}.with_fg(theme.icon)); // 
        if (query.empty()) put("Filter\xe2\x80\xa6", Style{}.with_fg(theme.prompt).with_italic());
        else               put(query, Style{}.with_fg(theme.query));
        put("\xe2\x96\x8f", Style{}.with_fg(theme.on)); // ▏ caret
        // count + toggles
        std::string cnt = "   " + (total > 0 ? std::to_string(matches) + "/" + std::to_string(total)
                                             : std::string("no matches")) + "   ";
        put(cnt, Style{}.with_fg(total > 0 ? theme.count : theme.no_match));
        toggle("Aa", case_sensitive);
        toggle(".\xe2\x88\x97", fuzzy); // .∗
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
