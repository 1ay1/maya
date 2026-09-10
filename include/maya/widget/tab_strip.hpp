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
    Color divider  = Color::bright_black();   // │ between tabs, and the rule
};

// How the selected tab is marked.
//
// A FAMILY, not one house style. The three differ in what they cost and what
// they emphasise, and a host picks by what its surface can afford:
enum class TabMark : std::uint8_t {
    // A solid rule under the active label. Reads as "this one" without
    // competing with a row cursor for the same visual channel — a filled
    // block does, and in a 16-colour terminal the two are easy to confuse.
    // Costs a second row. The default, and what a panel's tabs use.
    Underline,
    // A leading ◆ on the active tab. One row total, for a strip that has to
    // share a line budget with content.
    Dot,
    // The editor style: a leading ▎ accent bar on the active tab, " │ "
    // dividers between tabs, and a full-width rule beneath whose segment
    // under the active tab is lit.
    //
    // Denser than Underline and more emphatic than Dot — it says "these are
    // PEERS you switch between", which is what an open-file strip is, as
    // against "these are views of one thing". Costs two rows, and the
    // dividers cost columns, so it wants a wide surface.
    Editor,
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
            if (mark == TabMark::Dot)    w += 2;   // "◆ " / "  "
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
            // Editor mode separates tabs with a visible divider rather than
            // whitespace: its tabs are PEERS, and a rule between them reads
            // as "another one of the same kind" where a gap reads as "a
            // different thing".
            const int sep_w = mark == TabMark::Editor ? 5 : gap;
            auto span = [&](int lo, int hi) {
                int t = 0;
                for (int i = lo; i <= hi; ++i)
                    t += tab_width(tabs[static_cast<std::size_t>(i)])
                       + (i > lo ? sep_w : 0);
                return t;
            };
            int start = 0;
            // 2 columns reserved for the "… " chip once anything is hidden.
            while (start < act && span(start, act) > avail - (start > 0 ? 2 : 0))
                ++start;

            std::string labels;
            std::vector<StyledRun> label_runs;
            std::string rule;
            // Column cursor, so each piece advances the underline in step.
            int col = 0;
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

            for (int i = 0; i < indent; ++i) { labels += ' '; rule += ' '; ++col; }
            if (start > 0) {
                put("\xe2\x80\xa6 ", Style{}.with_fg(theme.ellipsis));
                rule_pad(2, false);
                col += 2;
            }

            for (int i = start; i < n; ++i) {
                const auto& t = tabs[static_cast<std::size_t>(i)];
                const bool on = (i == act);
                if (i > start) {
                    if (mark == TabMark::Editor) {
                        // "  │  " — symmetric air around the divider. The
                        // separator has to read as a boundary BETWEEN two
                        // labels, and padding it on one side only makes it
                        // look attached to the tab on that side.
                        put("  ", Style{});
                        put("\xe2\x94\x82", Style{}.with_fg(theme.divider));
                        put("  ", Style{});
                    } else {
                        put(std::string(static_cast<std::size_t>(gap), ' '), Style{});
                    }
                    rule_pad(sep_w, false);
                    col += sep_w;
                }

                if (mark == TabMark::Dot) {
                    put(on ? "\xe2\x97\x86 " : "  ", Style{}.with_fg(theme.accent));
                    rule_pad(2, false);
                    col += 2;
                }
                if (!t.dot_glyph.empty()) {
                    put(t.dot_glyph, Style{}.with_fg(t.dot_color));
                    put(" ", Style{});
                    const int w = unicode::str_width(t.dot_glyph) + 1;
                    rule_pad(w, false);
                    col += w;
                }
                Style ls = Style{}.with_fg(on ? theme.active : theme.idle);
                if (on) ls = ls.with_bold();
                put(t.label, ls);
                // ONLY the label is underlined. Extending the rule under a
                // status dot and a trailing diffstat made one tab wear three
                // disjoint underlines — it read as three marks rather than
                // one, which is the opposite of what a selection indicator
                // is for.
                rule_pad(unicode::str_width(t.label), on);
                col += unicode::str_width(t.label);

                if (!t.detail.empty()) {
                    put(" ", Style{});
                    put(t.detail, Style{}.with_fg(t.detail_color).with_dim());
                    const int w = unicode::str_width(t.detail) + 1;
                    rule_pad(w, false);
                    col += w;
                }
            }

            Element label_row{TextElement{.content = std::move(labels),
                                          .wrap    = TextWrap::TruncateEnd,
                                          .runs    = std::move(label_runs)}};
            // Dot and Editor are single-row marks. Editor distinguishes the
            // active tab by COLOUR alone — bold in the accent, its
            // neighbours dim — with │ dividers carrying the structure. No
            // rule and no fill: the dividers already say "separate tabs",
            // so a second marker on the same row is one signal too many.
            if (mark == TabMark::Dot || mark == TabMark::Editor)
                return label_row;

            return v(std::move(label_row),
                     Element{TextElement{.content = std::move(rule),
                                         .style   = Style{}.with_fg(theme.accent),
                                         .wrap    = TextWrap::TruncateEnd}}).build();

        }).build();
    }
};

}  // namespace maya
