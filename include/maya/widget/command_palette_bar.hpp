#pragma once
// maya::widget::CommandPalette — fuzzy command launcher (background-free)
//
// The Ctrl-Shift-P palette: a prompt showing the query, then a list of
// commands with the query's characters highlighted, an optional category, and
// a right-aligned keybinding chip. The selected row is softly shaded. Uses the
// FuzzyLine matcher for highlight positions.
//
// Usage:
//   CommandPalette p;
//   p.query = "fmt";
//   p.command({SymKind::Function, "Format Document", "Editor", "\u21e7\u2325 F"})
//    .command({SymKind::Function, "Format Selection", "Editor", ""})
//    .select(0);
//   Element ui = p | dsl::width(60);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "fuzzy_line.hpp"
#include "sym_kind.hpp"

namespace maya {

struct CommandPaletteTheme {
    Color border   = Color::hex(0x313244);
    Color prompt   = Color::hex(0x89B4FA); // > and query caret
    Color query    = Color::hex(0xCDD6F4);
    Color title     = Color::hex(0xBAC2DE);
    Color sel_title  = Color::hex(0xF5F5F7);
    Color match      = Color::hex(0xF9E2AF);
    Color category   = Color::hex(0x585B70);
    Color keybind    = Color::hex(0x9399B2);
    Color sel_shade  = Color::hex(0x313244);
};

struct CommandPalette {
    struct Command {
        SymKind     icon = SymKind::Function;
        std::string title;
        std::string category;
        std::string keybind;
    };

    std::string          query;
    std::vector<Command> commands;
    int                  selected_ = 0;
    int                  max_rows  = 8;
    CommandPaletteTheme  theme;

    CommandPalette& command(Command c) { commands.push_back(std::move(c)); return *this; }
    CommandPalette& select(int i)      { selected_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const int n   = static_cast<int>(commands.size());
        const int sel = n ? std::clamp(selected_, 0, n - 1) : 0;
        const int rows = std::min(max_rows, n);
        int top = std::clamp(sel - rows / 2, 0, std::max(0, n - rows));
        int bot = std::min(n, top + rows);

        std::vector<Element> col;

        // prompt row
        {
            std::string s; std::vector<StyledRun> r;
            auto put = [&](std::string_view t, Style st) {
                if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
            };
            put("\xef\x80\x82  ", Style{}.with_fg(theme.prompt)); //  search
            if (query.empty())
                put("Type a command\xe2\x80\xa6", Style{}.with_fg(theme.category).with_italic());
            else
                put(query, Style{}.with_fg(theme.query));
            put("\xe2\x96\x8f", Style{}.with_fg(theme.prompt)); // ▏ caret
            col.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                               .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }

        for (int i = top; i < bot; ++i)
            col.push_back(row(commands[static_cast<size_t>(i)], i == sel));
        if (bot < n)
            col.push_back(Element{TextElement{
                .content = "  \xe2\x96\xbc " + std::to_string(n - bot) + " more",
                .style = Style{}.with_fg(theme.category), .wrap = TextWrap::NoWrap }});

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(theme.border)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(col)).build());
    }

private:
    Element row(const Command& c, bool sel) const {
        const Color shade = sel ? theme.sel_shade : Color{};
        auto tint = [&](Style st) { return sel ? st.with_bg(shade) : st; };

        // left segment: pointer + icon + title (fuzzy highlighted) + category
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; r.push_back({s.size(), t.size(), tint(st)}); s += t;
        };
        put(sel ? "\xe2\x96\xb8 " : "  ", Style{}.with_fg(theme.prompt)); // ▸
        put(std::string(sym_glyph(c.icon)) + " ", Style{}.with_fg(sym_color(c.icon)));

        auto hits = FuzzyLine::match(c.title, query);
        const Style base = Style{}.with_fg(sel ? theme.sel_title : theme.title);
        const Style hit  = Style{}.with_fg(theme.match).with_bold();
        size_t mi = 0;
        for (size_t i = 0; i < c.title.size(); ++i) {
            bool m = (mi < hits.positions.size() && static_cast<int>(i) == hits.positions[mi]);
            if (m) ++mi;
            put(std::string_view{c.title}.substr(i, 1), m ? hit : base);
        }
        if (!c.category.empty())
            put("  " + c.category, Style{}.with_fg(theme.category));

        std::string left = std::move(s);
        std::vector<StyledRun> lruns = std::move(r);
        std::string key = c.keybind;

        return Element{ComponentElement{
            .render = [left = std::move(left), lruns = std::move(lruns),
                       key = std::move(key), sel, shade, kc = theme.keybind]
                      (int w, int) -> Element {
                std::string s = left; std::vector<StyledRun> runs = lruns;
                const Style fill = sel ? Style{}.with_bg(shade) : Style{};
                int lw = string_width(left), rw = string_width(key);
                int gap = std::max(1, w - lw - rw);
                runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                s.append(static_cast<size_t>(gap), ' ');
                if (!key.empty()) {
                    Style ks = Style{}.with_fg(kc);
                    if (sel) ks = ks.with_bg(shade);
                    runs.push_back({s.size(), key.size(), ks});
                    s += key;
                }
                int used = string_width(s);
                if (used < w) {
                    runs.push_back({s.size(), static_cast<size_t>(w - used), fill});
                    s.append(static_cast<size_t>(w - used), ' ');
                }
                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
        }};
    }
};

} // namespace maya
