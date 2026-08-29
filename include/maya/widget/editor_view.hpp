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
    int* scroll = nullptr;                       // persistent scroll-top (required for stable scroll)

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
        return { code_prefix(pane_h) + col, row - top };
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

        CodeView cv{src, {.lang = lang, .theme = theme, .first_line = lo + 1}};
        cv.set_caret(row + 1, col);
        if (sel) cv.set_selection(sr + 1, sc, er + 1, ec);
        return cv.build();
    }

    static int digits(int v) { int d = 1; for (; v >= 10; v /= 10) ++d; return d; }
};

} // namespace maya
