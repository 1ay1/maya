#pragma once
// maya::widget::SearchResults — grouped search matches (background-free)
//
// The results tree of a project-wide search: matches grouped by file, each
// file a header with a hit count, each match a line with its number and the
// matched substring highlighted in place (before / MATCH / after). Tree-guide
// connectors under files. Foreground-only.
//
// Usage:
//   SearchResults s;
//   s.file("src/rope.cpp", 2)
//     .match(41, "    return ", "weight", " + left->size();")
//     .match(50, "  std::size_t ", "weight", " = 0;")
//    .file("include/rope.hpp", 1)
//     .match(8,  "  std::size_t ", "weight", ";");
//   Element ui = s | dsl::width(60);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SearchResultsTheme {
    Color file    = Color::hex(0xCDD6F4);
    Color count   = Color::hex(0x585B70);
    Color guide   = Color::hex(0x45475A);
    Color lineno   = Color::hex(0x585B70);
    Color text     = Color::hex(0x9399B2);
    Color match     = Color::hex(0xF9E2AF); // highlighted hit
    Color active    = Color::hex(0x232634);
};

class SearchResults {
public:
    SearchResults& file(std::string name, int count) {
        groups_.push_back({std::move(name), count, {}}); return *this;
    }
    SearchResults& match(int line, std::string before, std::string hit, std::string after) {
        groups_.back().items.push_back({line, std::move(before), std::move(hit), std::move(after)});
        return *this;
    }
    SearchResults& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        int idx = 0;
        for (const auto& g : groups_) {
            rows.push_back(file_row(g));
            for (size_t j = 0; j < g.items.size(); ++j) {
                const bool last = (j + 1 == g.items.size());
                rows.push_back(match_row(g.items[j], last, idx == active_));
                ++idx;
            }
        }
        return dsl::v(std::move(rows)).build();
    }

private:
    struct Match { int line; std::string before, hit, after; };
    struct Group { std::string name; int count; std::vector<Match> items; };

    std::vector<Group> groups_;
    int                active_ = -1;
    SearchResultsTheme theme;

    Element file_row(const Group& g) const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        put("\xef\x84\x87 ", Style{}.with_fg(theme.guide));  //  chevron-down
        put("\xef\x85\x9b ", Style{}.with_fg(Color::hex(0x89B4FA))); //  file
        put(g.name, Style{}.with_fg(theme.file).with_bold());
        put("  " + std::to_string(g.count), Style{}.with_fg(theme.count));
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }

    Element match_row(const Match& m, bool last, bool on) const {
        std::string left; std::vector<StyledRun> lr;
        const Color shade = on ? theme.active : Color{};
        auto tint = [on, shade](Style st){ return on ? st.with_bg(shade) : st; };
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(), t.size(), tint(st)}); left += t; };
        put(last ? " \xe2\x94\x94 " : " \xe2\x94\x9c ", Style{}.with_fg(theme.guide)); // └ / ├
        std::string ln = std::to_string(m.line);
        while (ln.size() < 4) ln.insert(ln.begin(), ' ');
        put(ln + "  ", Style{}.with_fg(theme.lineno));
        put(m.before, Style{}.with_fg(theme.text));
        put(m.hit, Style{}.with_fg(theme.match).with_bold());
        put(m.after, Style{}.with_fg(theme.text));

        if (!on)
            return Element{TextElement{ .content = std::move(left), .style = Style{},
                                        .wrap = TextWrap::NoWrap, .runs = std::move(lr) }};
        return Element{ComponentElement{
            .render = [left = std::move(left), lr = std::move(lr), shade](int w, int) -> Element {
                std::string s = left; std::vector<StyledRun> runs = lr;
                int used = string_width(s);
                if (used < w) { runs.push_back({s.size(), static_cast<size_t>(w - used),
                                                Style{}.with_bg(shade)});
                                s.append(static_cast<size_t>(w - used), ' '); }
                return Element{TextElement{ .content = std::move(s), .style = Style{}.with_bg(shade),
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
