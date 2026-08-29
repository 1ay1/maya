#pragma once
// maya::widget::WhichKeyMenu — keybinding hint popup (background-free)
//
// The which-key / command-menu overlay: after a leader key, a grid of
// available "key → action" bindings, grouped-prefix entries marked with a +
// and accent so you can see which keys open further menus. Thin rounded box.
//
// Usage:
//   WhichKeyMenu m{"SPACE"};
//   m.key("f", "find file").key("g", "git", /*group=*/true)
//    .key("b", "buffers", true).key("w", "window", true)
//    .key("q", "quit").key("/", "search");
//   Element ui = m | dsl::width(48);

#include <algorithm>
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

struct WhichKeyTheme {
    Color title  = Color::hex(0xCBA6F7);
    Color key    = Color::hex(0xF9E2AF);
    Color arrow  = Color::hex(0x585B70);
    Color action = Color::hex(0xBAC2DE);
    Color group   = Color::hex(0x89B4FA); // entries that open a submenu
};

struct WhichKeyMenu {
    struct Entry { std::string key; std::string action; bool group; };

    std::string        title;
    std::vector<Entry> entries;
    int                columns = 3;
    WhichKeyTheme      theme;

    explicit WhichKeyMenu(std::string t = {}) : title(std::move(t)) {}

    WhichKeyMenu& key(std::string k, std::string action, bool group = false) {
        entries.push_back({std::move(k), std::move(action), group}); return *this;
    }
    WhichKeyMenu& cols(int c) { columns = c; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const int n = static_cast<int>(entries.size());
        const int cols = std::max(1, columns);
        const int rowsN = (n + cols - 1) / cols;

        // build one Element per cell, laid out column-major into `cols` columns
        std::vector<std::vector<Element>> columnsE(static_cast<size_t>(cols));
        for (int i = 0; i < n; ++i) {
            int c = i / rowsN; // column-major fill
            if (c >= cols) c = cols - 1;
            columnsE[static_cast<size_t>(c)].push_back(cell(entries[static_cast<size_t>(i)]));
        }

        std::vector<Element> colEls;
        for (auto& col : columnsE)
            colEls.push_back((dsl::v(std::move(col)) | dsl::grow(1)).build());

        std::vector<Element> body;
        if (!title.empty())
            body.push_back(Element{TextElement{
                .content = title, .style = Style{}.with_fg(theme.title).with_bold(),
                .wrap = TextWrap::NoWrap }});
        body.push_back((dsl::h(std::move(colEls)) | dsl::gap(2)).build());

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(Color::hex(0x313244))
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(body)).build());
    }

private:
    Element cell(const Entry& e) const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        put(e.key, Style{}.with_fg(theme.key).with_bold());
        put(" \xe2\x86\x92 ", Style{}.with_fg(theme.arrow)); // →
        if (e.group) put("+", Style{}.with_fg(theme.group).with_bold());
        put(e.action, Style{}.with_fg(e.group ? theme.group : theme.action));
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
