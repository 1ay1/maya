#pragma once
// maya::widget::EditorTabBar — editor file-tab strip (background-free)
//
// A polished row of open-file tabs with everything a real editor shows:
//   • a per-language filetype glyph, tinted to the language's colour
//   • a modified dot ● / a close × on the active tab / a diagnostic dot
//   • pinned () and preview (italic) tabs
//   • an accent underline under the active tab (VSCode-style indicator)
//   • horizontal overflow: the strip windows around the active tab and shows
//     ‹N / N› chevrons for how many tabs are hidden on each side
//
// Foreground-only — the terminal keeps its own background throughout.
//
// Usage:
//   EditorTabBar tb;
//   tb.tab("main.cpp", true)                       // modified
//     .tab({.name = "rope.hpp", .diag = TabDiag::Error})
//     .tab({.name = "notes.md", .preview = true})
//     .active(0);
//   Element ui = tb;

#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class TabDiag : uint8_t { None, Error, Warning, Info };

struct EditorTabBarTheme {
    Color active   = Color::hex(0xF5F5F7); // active tab name
    Color inactive = Color::hex(0x6C7086); // idle tab name
    Color accent   = Color::hex(0x89B4FA); // active bar + underline
    Color modified = Color::hex(0xF9E2AF); // ● dirty dot
    Color close    = Color::hex(0x9399B2); // × close glyph
    Color divider  = Color::hex(0x313244); // │ between tabs
    Color chevron  = Color::hex(0x585B70); // ‹N / N› overflow hints
    Color pin       = Color::hex(0x94E2D5); //  pinned glyph
    Color err       = Color::hex(0xF38BA8);
    Color warn      = Color::hex(0xF9E2AF);
    Color info       = Color::hex(0x89B4FA);
};

struct EditorTabBarConfig {
    bool underline = true; // draw the accent underline row under the active tab
};

struct EditorTabBar {
    struct Tab {
        std::string name;
        bool        modified = false;
        TabDiag     diag     = TabDiag::None;
        bool        pinned   = false;
        bool        preview  = false; // italic, like a single-click preview tab
    };

    EditorTabBarConfig config{};
    std::vector<Tab>   tabs;
    int                active_ = 0;
    EditorTabBarTheme  theme;

    EditorTabBar& tab(std::string name, bool modified = false) {
        tabs.push_back({std::move(name), modified, TabDiag::None, false, false});
        return *this;
    }
    EditorTabBar& tab(Tab t) { tabs.push_back(std::move(t)); return *this; }
    EditorTabBar& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Pre-render each tab into a self-contained cell (text + runs + width).
        std::vector<Cell> cells;
        cells.reserve(tabs.size());
        for (size_t i = 0; i < tabs.size(); ++i)
            cells.push_back(make_cell(tabs[i], static_cast<int>(i) == active_));

        return Element{ComponentElement{
            .render = [cells = std::move(cells), act = active_,
                       th = theme, cfg = config](int w, int) -> Element {
                return layout(cells, act, th, cfg, w);
            },
            .measure = [rows = config.underline ? 2 : 1](int max_w) -> Size {
                return Size{Columns(max_w), Rows(rows)};
            },
        }};
    }

private:
    struct Cell {
        std::string            s;
        std::vector<StyledRun> runs;
        int                    w = 0;
        bool                   active = false;
    };

    static void put(std::string& s, std::vector<StyledRun>& runs,
                    std::string_view t, Style st) {
        if (t.empty()) return;
        runs.push_back({s.size(), t.size(), st});
        s += t;
    }

    Cell make_cell(const Tab& t, bool on) const {
        Cell c; c.active = on;
        const Style icon_st = Style{}.with_fg(on ? color_for(t.name) : theme.inactive);
        Style name_st = Style{}.with_fg(on ? theme.active : theme.inactive);
        if (on) name_st = name_st.with_bold();
        if (t.preview) name_st = name_st.with_italic();

        put(c.s, c.runs, on ? "\xe2\x96\x8e " : "  ",        // ▎ active bar
            Style{}.with_fg(theme.accent));
        if (t.pinned)
            put(c.s, c.runs, "\xef\x82\x8d ", Style{}.with_fg(theme.pin)); // 
        put(c.s, c.runs, std::string(glyph_for(t.name)) + " ", icon_st);
        put(c.s, c.runs, t.name, name_st);

        // trailing badge: modified > diagnostic > close(active) > none
        if (t.modified)
            put(c.s, c.runs, " \xe2\x97\x8f", Style{}.with_fg(theme.modified));  // ●
        else if (t.diag != TabDiag::None)
            put(c.s, c.runs, " \xe2\x97\x8f", Style{}.with_fg(diag_color(t.diag)));
        else if (on)
            put(c.s, c.runs, " \xc3\x97", Style{}.with_fg(theme.close));         // ×
        else
            put(c.s, c.runs, "  ", Style{});
        put(c.s, c.runs, " ", Style{});

        c.w = string_width(c.s);
        return c;
    }

    // Window the cells around the active tab to fit `w`, then compose the
    // label row (and, optionally, the accent-underline row beneath it).
    static Element layout(const std::vector<Cell>& cells, int act,
                          const EditorTabBarTheme& th,
                          const EditorTabBarConfig& cfg, int w) {
        const int n = static_cast<int>(cells.size());
        if (n == 0) return Element{TextElement{}};
        act = std::clamp(act, 0, n - 1);
        const int DIV = 3; // " │ "

        int lo = act, hi = act, used = cells[act].w;
        const int budget = std::max(cells[act].w, w - 6); // reserve for chevrons
        while (true) {
            bool did = false;
            if (hi + 1 < n && used + DIV + cells[hi + 1].w <= budget) {
                used += DIV + cells[++hi].w; did = true;
            }
            if (lo - 1 >= 0 && used + DIV + cells[lo - 1].w <= budget) {
                used += DIV + cells[--lo].w; did = true;
            }
            if (!did) break;
        }

        std::string row; std::vector<StyledRun> runs;
        int col = 0, act_col = 0, act_w = 0;
        auto emit = [&](std::string_view t, Style st, int width) {
            put(row, runs, t, st); col += width;
        };

        if (lo > 0)
            emit("\xe2\x80\xb9" + std::to_string(lo) + " ",       // ‹N
                 Style{}.with_fg(th.chevron), 2 + digits(lo));

        for (int i = lo; i <= hi; ++i) {
            if (i > lo) emit(" \xe2\x94\x82 ", Style{}.with_fg(th.divider), DIV); // │
            if (i == act) { act_col = col; act_w = cells[i].w; }
            const size_t base = row.size();
            row += cells[i].s;
            for (const auto& r : cells[i].runs)
                runs.push_back({base + r.byte_offset, r.byte_length, r.style});
            col += cells[i].w;
        }

        if (hi < n - 1) {
            int hidden = n - 1 - hi;
            emit(" \xe2\x80\xba" + std::to_string(hidden),        // ›N
                 Style{}.with_fg(th.chevron), 2 + digits(hidden));
        }

        Element label{TextElement{
            .content = std::move(row), .style = Style{},
            .wrap = TextWrap::NoWrap, .runs = std::move(runs),
        }};
        if (!cfg.underline) return label;

        // Underline row: a full-width dim rule that reads as a tab-strip
        // separator, with the segment under the active tab in the accent
        // colour. Filling the whole width avoids a blank-looking gap.
        Element underline{ComponentElement{
            .render = [act_col, act_w, rule = th.divider, accent = th.accent]
                      (int w, int) -> Element {
                std::string s; std::vector<StyledRun> runs;
                for (int c = 0; c < w; ++c) {
                    bool on = (c >= act_col && c < act_col + act_w);
                    const size_t off = s.size();
                    s += on ? "\xe2\x94\x81" : "\xe2\x94\x80"; // ━ accent / ─ dim
                    runs.push_back({off, 3, Style{}.with_fg(on ? accent : rule)});
                }
                return Element{TextElement{ .content = std::move(s), .style = Style{},
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
        return dsl::v(label, underline).build();
    }

    static int digits(int n) { int d = 1; for (; n >= 10; n /= 10) ++d; return d; }

    Color diag_color(TabDiag d) const {
        switch (d) {
            case TabDiag::Error:   return theme.err;
            case TabDiag::Warning: return theme.warn;
            case TabDiag::Info:    return theme.info;
            default:               return theme.inactive;
        }
    }

    static const char* glyph_for(std::string_view name) {
        auto ends = [&](std::string_view e) {
            return name.size() >= e.size() &&
                   name.compare(name.size() - e.size(), e.size(), e) == 0;
        };
        if (ends(".cpp") || ends(".cc") || ends(".hpp") || ends(".h") ||
            ends(".cxx")) return "\xee\x98\x9d"; //  C++
        if (ends(".rs"))   return "\xee\x9e\xa8"; //  Rust
        if (ends(".py"))   return "\xee\x98\x86"; //  Python
        if (ends(".go"))   return "\xee\x98\xa7"; //  Go
        if (ends(".ts"))   return "\xee\x98\xa8"; //  TS
        if (ends(".js"))   return "\xee\x9e\x8e"; //  JS
        if (ends(".json")) return "\xee\x98\x8b"; //  JSON
        if (ends(".md"))   return "\xef\x92\x89"; //  Markdown
        return "\xef\x85\x9b"; //  generic file
    }

    // Language brand colours for the active-tab icon.
    static Color color_for(std::string_view name) {
        auto ends = [&](std::string_view e) {
            return name.size() >= e.size() &&
                   name.compare(name.size() - e.size(), e.size(), e) == 0;
        };
        if (ends(".cpp") || ends(".cc") || ends(".hpp") || ends(".h") ||
            ends(".cxx")) return Color::hex(0x649AD2); // C++ blue
        if (ends(".rs"))   return Color::hex(0xDEA584); // Rust
        if (ends(".py"))   return Color::hex(0xFFD43B); // Python
        if (ends(".go"))   return Color::hex(0x00ADD8); // Go
        if (ends(".ts"))   return Color::hex(0x3178C6); // TS
        if (ends(".js"))   return Color::hex(0xF7DF1E); // JS
        if (ends(".json")) return Color::hex(0xCBCB41);
        if (ends(".md"))   return Color::hex(0x89B4FA);
        return Color::hex(0x89B4FA);
    }
};

} // namespace maya
