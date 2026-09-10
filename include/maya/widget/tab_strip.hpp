#pragma once
// maya::TabStrip — one horizontal strip of selectable views.
//
// The chrome a panel wears when it shows SEVERAL views of one subject: the
// stats viewer's areas, the diff reviewer's changed files. Not a general
// "row of labels" — a strip whose job is to answer "which view am I in, and
// what else is there" without ever pushing the answer off-screen.
//
// ── Why this is a widget and not a helper in each host ───────────────────
//
// The naive version is ten lines and every host can write it. The version
// that survives contact with a real changeset is not, and three properties
// are what separate them:
//
//   1. It SCROLLS. A 40-file review is wider than any pane, so the strip
//      slides to keep the active tab visible and collapses what it passed
//      into a leading ellipsis. Without this, moving to a tab can move
//      focus to something rendered entirely off-screen — the user is
//      navigating blind.
//   2. Tabs carry STATUS. A file is accepted / rejected / pending, and the
//      dot that says so is part of the tab, not a second widget beside it.
//   3. Tabs carry a TRAILING detail (a diffstat, a count). It has to
//      participate in the same width arithmetic, or the scroll maths is
//      wrong by exactly the part the host added itself.
//
// Hand-rolled per host, those three drift — and the two that drift silently
// (scrolling and width) are the two that break at exactly the size where a
// user needs the strip most.
//
// Usage:
//   TabStrip s;
//   s.tab("Smart Mode").tab("Tools");
//   s.active(0);
//   Element ui = s;
//
//   // with status + detail (the diff reviewer's shape)
//   TabStrip files;
//   files.tab("rope.cpp").dot(Color::green()).detail("+12 -3", …);
//   files.active(2);

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../text/unicode_width.hpp"

namespace maya {

struct TabStripTheme {
    Color active   = Color::bright_white();   // the selected label
    Color idle     = Color::bright_black();   // every other label
    Color accent   = Color::cyan();           // the underline / marker
    Color detail   = Color::bright_black();   // trailing count / diffstat
    Color ellipsis = Color::bright_black();   // the "…" scrolled-past chip
};

// How the selected tab is marked.
enum class TabMark : std::uint8_t {
    // A solid rule under the active label. Reads as "this one" without
    // competing with a row cursor for the same visual channel — a filled
    // block does, and in a 16-colour terminal the two are easy to confuse.
    // Costs a second row.
    Underline,
    // A leading ◆ on the active tab. One row total, for a strip that has to
    // share a line budget with content.
    Dot,
};

struct TabStrip {
    struct Tab {
        std::string label;
        // Optional leading status dot. Empty glyph = no dot. The COLOUR is
        // the signal; the glyph is a knob so a host can use ● / ○ / ✓ to
        // stay legible without colour.
        std::string dot_glyph;
        Color       dot_color = Color::bright_black();
        // Optional trailing detail: a diffstat, a count, a size. Rendered
        // dim after the label and INCLUDED in the width arithmetic, which
        // is the part a host doing this itself gets wrong.
        std::string detail;
        Color       detail_color = Color::bright_black();
    };

    std::vector<Tab> tabs;
    int              active_index = 0;
    TabStripTheme    theme;
    TabMark          mark = TabMark::Underline;
    // Columns of lead-in, so the strip lines up with whatever is above it
    // (the panel family indents its subtitle by 2).
    int              indent = 2;
    // Columns between tabs.
    int              gap = 3;

    TabStrip& tab(std::string label) {
        tabs.push_back({std::move(label), {}, {}, {}, {}});
        return *this;
    }
    // Attach a status dot to the most recently added tab.
    TabStrip& dot(Color c, std::string glyph = "\xe2\x97\x8f") {   // ●
        if (!tabs.empty()) {
            tabs.back().dot_glyph = std::move(glyph);
            tabs.back().dot_color = c;
        }
        return *this;
    }
    // Attach a trailing detail to the most recently added tab.
    TabStrip& detail(std::string text, Color c) {
        if (!tabs.empty()) {
            tabs.back().detail       = std::move(text);
            tabs.back().detail_color = c;
        }
        return *this;
    }
    TabStrip& active(int i)          { active_index = i; return *this; }
    TabStrip& marker(TabMark m)      { mark = m;         return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (tabs.empty()) return Element{TextElement{}};

        // Width of one whole tab, in DISPLAY COLUMNS — dot, label and
        // detail together. Columns, not bytes: a label with any non-ASCII
        // in it would otherwise scroll and underline at the wrong place.
        auto tab_width = [&](const Tab& t) {
            int w = unicode::str_width(t.label);
            if (!t.dot_glyph.empty()) w += unicode::str_width(t.dot_glyph) + 1;
            if (!t.detail.empty())    w += unicode::str_width(t.detail) + 1;
            if (mark == TabMark::Dot) w += 2;   // "◆ " / "  " marker column
            return w;
        };

        // Width-aware, because the whole point is fitting the ACTIVE tab.
        // The strip is a single text row (two with an underline), so the
        // component's cost is a few string appends — not a subtree rebuild.
        const int n = static_cast<int>(tabs.size());
        const int act = active_index < 0 ? 0 : (active_index >= n ? n - 1 : active_index);

        return component([tabs = tabs, theme = theme, mark = mark,
                          indent = indent, gap = gap, act, n,
                          tab_width](int avail_w, int) -> Element {
            const int avail = avail_w > indent ? avail_w - indent : 0;

            // Slide the window right until the ACTIVE tab's right edge fits.
            // This is the property a hand-rolled strip loses: without it,
            // selecting a tab can select something drawn off-screen.
            auto span = [&](int lo, int hi) {
                int t = 0;
                for (int i = lo; i <= hi; ++i)
                    t += tab_width(tabs[static_cast<std::size_t>(i)])
                       + (i > lo ? gap : 0);
                return t;
            };
            int start = 0;
            // 2 columns reserved for the "… " chip once anything is hidden.
            while (start < act && span(start, act) > avail - (start > 0 ? 2 : 0))
                ++start;

            std::string labels;
            std::vector<StyledRun> label_runs;
            std::string rule;
            auto put = [&](std::string_view s, Style st) {
                if (s.empty()) return;
                label_runs.push_back({labels.size(), s.size(), st});
                labels += s;
            };
            // The underline row is built in lockstep with the label row so
            // the two cannot disagree about where a tab starts and ends.
            auto rule_pad = [&](int cols, bool solid) {
                for (int i = 0; i < cols; ++i) rule += solid ? "\xe2\x94\x81" : " ";
            };

            for (int i = 0; i < indent; ++i) { labels += ' '; rule += ' '; }
            if (start > 0) {
                put("\xe2\x80\xa6 ", Style{}.with_fg(theme.ellipsis));
                rule_pad(2, false);
            }

            for (int i = start; i < n; ++i) {
                const auto& t = tabs[static_cast<std::size_t>(i)];
                const bool on = (i == act);
                if (i > start) { put(std::string(static_cast<std::size_t>(gap), ' '), Style{}); rule_pad(gap, false); }

                if (mark == TabMark::Dot) {
                    put(on ? "\xe2\x97\x86 " : "  ", Style{}.with_fg(theme.accent));
                    rule_pad(2, false);
                }
                if (!t.dot_glyph.empty()) {
                    put(t.dot_glyph, Style{}.with_fg(t.dot_color));
                    put(" ", Style{});
                    rule_pad(unicode::str_width(t.dot_glyph) + 1, on);
                }
                Style ls = Style{}.with_fg(on ? theme.active : theme.idle);
                if (on) ls = ls.with_bold();
                put(t.label, ls);
                rule_pad(unicode::str_width(t.label), on);

                if (!t.detail.empty()) {
                    put(" ", Style{});
                    put(t.detail, Style{}.with_fg(t.detail_color).with_dim());
                    rule_pad(unicode::str_width(t.detail) + 1, on);
                }
            }

            Element label_row{TextElement{.content = std::move(labels),
                                          .wrap    = TextWrap::TruncateEnd,
                                          .runs    = std::move(label_runs)}};
            if (mark == TabMark::Dot) return label_row;
            return v(std::move(label_row),
                     Element{TextElement{.content = std::move(rule),
                                         .style   = Style{}.with_fg(theme.accent),
                                         .wrap    = TextWrap::TruncateEnd}}).build();
        }).build();
    }
};

}  // namespace maya
