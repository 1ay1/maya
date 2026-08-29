#pragma once
// maya::widget::EditorView — a scrolling, caret-tracking code viewport.
//
// CodeView renders a fixed block of source. EditorView is the editor surface:
// it holds the whole buffer (as a non-owning view of lines), a cursor and an
// optional selection, and a persistent scroll position, and paints only the
// visible window through CodeView — scrolling to keep the caret on screen.
//
// It's a pure VIEW of editor state (a host's text engine owns the buffer): the
// caller feeds it `lines`, `row`, `col`, an optional selection, and a pointer
// to where the scroll position lives (so it survives across frames). It also
// reports the caret's cell within the pane, so a host can float a completion /
// hover popup exactly at the caret via maya::with_float.
//
// Usage (in a Program::view):
//   maya::EditorView ev;
//   ev.lines  = std::span{doc.lines()};
//   ev.row    = cursor.row; ev.col = cursor.col;
//   ev.lang   = syntax::Lang::Cpp;
//   ev.scroll = &host_scroll_top;            // persistent int
//   Element pane = ev.build();               // in the layout
//   auto [cx, cy] = ev.caret(pane_h);        // to anchor a popup

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "code_view.hpp"

namespace maya {

struct EditorView {
    std::span<const std::string> lines;         // the whole buffer (non-owning)
    int  row = 0, col = 0;                       // cursor, 0-based (col in bytes)
    bool sel = false;                            // selection present?
    int  sr = 0, sc = 0, er = 0, ec = 0;         // ordered selection, 0-based
    syntax::Lang  lang  = syntax::Lang::Generic;
    CodeViewTheme theme = {};
    int tab_width = 4;
    int* scroll = nullptr;                       // persistent scroll-top (required for stable scroll)

    // Additional carets / selections for multi-cursor (the primary is row/col
    // + sel above; these are the rest). row/col stays the anchor for scroll.
    struct XCaret { int row, col; };
    struct XSel    { int sr, sc, er, ec; };
    std::vector<XCaret> extra_carets;
    std::vector<XSel>   extra_sels;

    // Per-line change marks (0-based row → mark) painted in the far-left git
    // ribbon. Empty by default; the host fills it from a diff.
    std::vector<std::pair<int, LineMark>> line_marks;

    struct Caret { int col; int row; };          // cell within the pane

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return Element{ComponentElement{
            .render  = [self = *this](int, int ph) -> Element { return self.paint(ph); },
            .measure = [](int mw) -> Size { return {Columns(mw), Rows(1)}; },
        }};
    }

    // The caret's (col,row) within the pane, at a given pane height. Pure —
    // resolves the same scroll the next paint will use.
    [[nodiscard]] Caret caret(int pane_h) const {
        const int top = resolve(std::max(1, pane_h));
        return { code_prefix(pane_h) + caret_display_col(), row - top };
    }

    // Display cell of the caret WITHIN its line: expands tabs to the tab stop
    // and counts wide (CJK/emoji) glyphs as 2 cells, so a popup anchored here
    // lands on the real screen column even past wide chars / tabs.
    [[nodiscard]] int caret_display_col() const {
        if (row < 0 || row >= static_cast<int>(lines.size())) return col;
        std::string_view l{lines[static_cast<std::size_t>(row)]};
        int cell = 0; std::size_t i = 0;
        const int cap = std::min<int>(col, static_cast<int>(l.size()));
        while (static_cast<int>(i) < cap) {
            if (l[i] == '\t') { cell += tab_width - (cell % tab_width); ++i; continue; }
            unsigned char lead = static_cast<unsigned char>(l[i]);
            int len = (lead < 0x80) ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                    : (lead >> 3) == 0x1E ? 4 : 1;
            char32_t cp = lead;
            if (len > 1) {
                cp = lead & (0x7F >> len);
                for (int k = 1; k < len && i + static_cast<std::size_t>(k) < l.size(); ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(l[i + static_cast<std::size_t>(k)]) & 0x3F);
            }
            cell += unicode::char_width(cp, unicode::WidthMode::Modern);
            i += static_cast<std::size_t>(len);
        }
        return cell;
    }

    // Number of leading columns CodeView draws before the code (ribbon + gutter).
    [[nodiscard]] int code_prefix(int pane_h) const {
        const int top = resolve(std::max(1, pane_h));
        const int n = static_cast<int>(lines.size());
        const int visible = std::min(n - top, std::max(1, pane_h));
        const int gutter_w = std::max(3, digits(std::max(1, top + visible)));
        return 1 /*ribbon*/ + gutter_w + 3 /*space+rule+space*/;
    }

private:
    mutable int internal_scroll_ = 0;

    int& scroll_ref() const { return scroll ? *scroll : internal_scroll_; }

    int resolve(int pane_h) const {
        int& top = scroll_ref();
        const int n = static_cast<int>(lines.size());
        if (row < top) top = row;
        else if (row >= top + pane_h) top = row - pane_h + 1;
        top = std::clamp(top, 0, std::max(0, n - pane_h));
        return top;
    }

    Element paint(int pane_h_in) const {
        const int pane_h = std::max(1, pane_h_in);
        const int top = resolve(pane_h);
        const int n = static_cast<int>(lines.size());
        const int lo = top, hi = std::min(n, top + pane_h);

        std::string src;
        for (int i = lo; i < hi; ++i) { if (i > lo) src += '\n'; src += lines[static_cast<std::size_t>(i)]; }

        CodeView cv{src, {.lang = lang, .theme = theme, .first_line = lo + 1, .tab_width = tab_width}};
        cv.set_caret(row + 1, col);
        for (const auto& c : extra_carets) cv.add_caret(c.row + 1, c.col);
        if (sel) cv.set_selection(sr + 1, sc, er + 1, ec);
        for (const auto& s : extra_sels) cv.add_selection(s.sr + 1, s.sc, s.er + 1, s.ec);
        for (const auto& [r, m] : line_marks)
            if (r >= lo && r < hi) cv.mark(r + 1, m);
        return cv.build();
    }

    static int digits(int v) { int d = 1; for (; v >= 10; v /= 10) ++d; return d; }
};

} // namespace maya
