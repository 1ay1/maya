#pragma once
// maya::widget::FindReplaceBar — search / replace bar (background-free)
//
// The floating find bar: a query field, a live match count (x/y), previous /
// next arrows, and the case / whole-word / regex toggles that light up when
// active. An optional second row adds the replace field with Replace / All
// actions. Purely presentational — you own the text and match state.
//
// Foreground-only; a thin rounded border frames it.
//
// Usage:
//   FindReplaceBar f;
//   f.query = "rope"; f.current = 3; f.total = 17;
//   f.case_sensitive = true; f.regex = true;
//   f.replace_mode = true; f.replacement = "Rope";
//   Element ui = f;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct FindReplaceTheme {
    Color border   = Color::hex(0x313244);
    Color label    = Color::hex(0x9399B2); // field text
    Color prompt   = Color::hex(0x585B70); // placeholder / icons
    Color count     = Color::hex(0xBAC2DE); // x/y match count
    Color no_match  = Color::hex(0xF38BA8); // count when 0 results
    Color toggle_on = Color::hex(0x89B4FA); // active toggle
    Color toggle_off= Color::hex(0x45475A); // inactive toggle
    Color action    = Color::hex(0xA6E3A1); // Replace / All buttons
};

struct FindReplaceBar {
    std::string query;
    std::string replacement;
    int         current = 0;   // 1-based index of the focused match (0 = none)
    int         total   = 0;   // total matches

    bool case_sensitive = false;
    bool whole_word     = false;
    bool regex          = false;
    bool replace_mode   = false;

    FindReplaceTheme theme;

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.push_back(find_row());
        if (replace_mode) rows.push_back(replace_row());

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(theme.border)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(rows)).build());
    }

private:
    static void put(std::string& s, std::vector<StyledRun>& r,
                    std::string_view t, Style st) {
        if (t.empty()) return;
        r.push_back({s.size(), t.size(), st});
        s += t;
    }

    Element field(std::string_view value, std::string_view placeholder) const {
        std::string s; std::vector<StyledRun> r;
        if (value.empty())
            put(s, r, placeholder, Style{}.with_fg(theme.prompt).with_italic());
        else
            put(s, r, value, Style{}.with_fg(theme.label));
        put(s, r, "\xe2\x96\x8f", Style{}.with_fg(theme.toggle_on)); // ▏ caret
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }

    Element find_row() const {
        std::string s; std::vector<StyledRun> r;
        put(s, r, "\xef\x80\x82  ", Style{}.with_fg(theme.prompt)); //  search

        // count
        std::string cnt = total > 0 ? std::to_string(current) + "/" + std::to_string(total)
                                     : "No results";
        put(s, r, "  " + cnt + "  ", Style{}.with_fg(total > 0 ? theme.count : theme.no_match));

        // nav arrows
        put(s, r, "\xef\x81\xb7 ", Style{}.with_fg(theme.prompt)); //  up
        put(s, r, "\xef\x81\xb8  ", Style{}.with_fg(theme.prompt)); //  down

        // toggles
        toggle(s, r, "Aa", case_sensitive);
        toggle(s, r, "\xef\x80\xa2", whole_word); //  whole word
        toggle(s, r, ".\xe2\x88\x97", regex);      // .∗ regex

        Element controls{TextElement{ .content = std::move(s), .style = Style{},
                                      .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
        // query field grows, controls sit at the right
        return dsl::h(field(query, "Find"), dsl::spacer(), controls).build();
    }

    Element replace_row() const {
        std::string s; std::vector<StyledRun> r;
        put(s, r, "\xef\x83\x94  ", Style{}.with_fg(theme.prompt)); //  replace
        put(s, r, "Replace", Style{}.with_fg(theme.action));
        put(s, r, "  ", Style{});
        put(s, r, "All", Style{}.with_fg(theme.action).with_bold());
        Element controls{TextElement{ .content = std::move(s), .style = Style{},
                                      .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
        return dsl::h(field(replacement, "Replace"), dsl::spacer(), controls).build();
    }

    void toggle(std::string& s, std::vector<StyledRun>& r,
                std::string_view lbl, bool on) const {
        Style st = Style{}.with_fg(on ? theme.toggle_on : theme.toggle_off);
        if (on) st = st.with_bold();
        put(s, r, lbl, st);
        put(s, r, " ", Style{});
    }
};

} // namespace maya
