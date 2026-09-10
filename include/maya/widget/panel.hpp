#pragma once
// maya::Panel — THE overlay widget. One container, one Row, one renderer.
//
// Picker and Form were two implementations of the same thing: a bordered,
// padded, titled box holding a scrollable list of rows, each row a leading
// cell and a trailing cell with a cursor bar in column 0. Every fix to one had
// to be repeated in the other, and the ones that were not became bugs — the
// form spilled past its border, painted a theme-coloured selection slab, and
// lost its help line off the bottom edge, all of which the picker had solved
// years earlier.
//
// An earlier attempt kept both row renderers and shared only the frame. That
// was worse: it needed a `scroll_resolved` flag so two auto-scroll
// implementations would not fight over the same offset — a flag whose whole
// job is to say "don't run your logic, mine already ran". A referee between
// two owners is not single ownership.
//
// So there is ONE Row. What used to distinguish a picker row from a form row
// is per-cell decoration, not structure:
//
//     edge · badge · leading [· highlight] · gap · trailing [· origin]
//
// A picker row fills `leading`/`trailing`; a form row additionally sets
// `control` and `origin`. Nothing about the frame, the scroll, the selection
// or the width arithmetic differs, because there is only one of each.
//
// ── Two invariants the layout depends on ────────────────────────────────
//
// 1. ONE SCREEN ROW PER ENTRY. maya applies scroll offsets in rows, so if an
//    entry could be two rows tall the cursor index and the scroll offset would
//    be in different units and auto-scroll would drift. Rows that own extra
//    lines (a help line, an error, an open dropdown) emit them as their own
//    entries and report the span.
//
// 2. FLEX, NEVER ARITHMETIC. Cells are laid out by the flex box at
//    `width(percent(100))`. Every previous version measured the row and padded
//    it — `w - text - reserve` — and each reserve constant was a second owner
//    of a width the layout already knew. They drifted, values slid under the
//    scrollbar, and "fix the reserve" became a sequence of magic numbers.

// ── Modularity ──────────────────────────────────────────────────────────
// The family lives in widget/panel/, one file per concern:
//
//   panel/theme.hpp    — the palette
//   panel/context.hpp  — ItemCtx: the ONLY facts an item widget may know
//   panel/caret.hpp    — caret splicing for line-edited items
//   panel/item/*.hpp   — ONE WIDGET PER ITEM KIND (value struct + renderer)
//   panel/control.hpp  — the closed variant over the item kinds
//   panel/item.hpp     — the one Item
//   panel/menu.hpp     — the inline Choice dropdown
//   panel/config.hpp   — everything a host supplies
//
// This header is the umbrella: the Panel class (frame, viewport, scroll,
// selection — the CONTAINER concerns) plus compatibility aliases so
// `maya::Panel::Row` and friends keep meaning what they always did.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "panel/config.hpp"
#include "scrollbar.hpp"

namespace maya {

namespace panel::detail {
// The terminal's width, or 0 when it cannot be known. Defined in panel.cpp
// beside the min-width clamp that also needs it, so ONE place decides how
// this is answered (ioctl, then COLUMNS when there is no tty).
[[nodiscard]] int terminal_cols() noexcept;
} // namespace panel::detail

class Panel {
public:
    // The family's types. `Item` is the name; `Row` is the pre-rename
    // compatibility spelling and new code should not use it.
    using Item   = panel::Item;
    using Row    = panel::Item;
    using Menu   = panel::Menu;
    using Config = panel::Config;

    explicit Panel(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const;

    // U+258E LEFT ONE QUARTER BLOCK — the cursor bar in column 0.
    static constexpr const char* kEdgeBar = "\xe2\x96\x8e";

    // Columns an EDITED value may occupy before it scrolls horizontally under
    // its caret. Derived from min_width — which the HOST already clamps to
    // the terminal — never measured inside the flex layout (asking for the
    // true width in there is what led to the reserve-constant bugs). The
    // floor keeps narrow panels usable; the deduction leaves room for the
    // marker lane, label and gaps. The layout still clips whatever exceeds
    // it — the budget only decides WHERE the window sits, never how much
    // space the cell gets.
    [[nodiscard]] int edit_budget() const noexcept {
        return std::max(34, cfg_.min_width - 26);
    }

    // Columns a DRAWN control (a bar, a meter, a strip) may occupy.
    //
    // Same derivation as edit_budget and for the same reason, which that
    // comment states: never measured inside the flex layout, because a
    // scroll viewport measures children against an unbounded width and a
    // control that asked would answer 2^24, publish it as the panel's
    // horizontal extent, and dirty the scroll state on every resize.
    // min_width is a fact known before layout — and the HOST already clamps
    // it to the terminal, so it tracks the real surface without being
    // measured against it.
    //
    // The deduction is larger than edit_budget's because a drawn control
    // shares its line with a label AND a value, where an edited one has the
    // rest of the row: label lane + both gaps + the value column. The floor
    // keeps a narrow panel drawing SOMETHING rather than nothing — the
    // per-kind clamp in ItemCtx::drawn_cells decides what that means for
    // each picture.
    // From the TERMINAL, not from min_width. That distinction cost a
    // debugging round and is worth stating: min_width is a FLOOR, and a
    // panel stretches past it to fill its container — so deriving a width
    // budget from it yields the same number on a 60-column pane and a
    // 200-column one, which is exactly the frozen-picture bug this exists
    // to fix. edit_budget can use min_width because it only decides WHERE
    // a scrolling window sits inside a cell, not how big the cell is.
    //
    // Still never measured inside the flex layout, which is the trap the
    // sibling's comment names: a scroll viewport measures its children
    // against an unbounded width, so a control that asked the LAYOUT would
    // answer 2^24 and dirty the scroll state on every resize. The terminal
    // width is known BEFORE layout, so it cannot join that loop.
    //
    // The deduction is the row's other columns: chrome, the label lane,
    // both gaps and the value cell.
    [[nodiscard]] int draw_budget() const noexcept {
        const int term = panel::detail::terminal_cols();
        const int usable = (term > 0 ? term : cfg_.min_width) - 40;
        return usable < 8 ? 8 : usable;
    }

    // Columns the VALUE cell occupies, as a max over every item.
    //
    // The one fact a row cannot know about itself, and the reason a table
    // is not a list of independent rows: numbers that do not end in the
    // same column cannot be compared down the page. Only the thing that
    // can see every row can answer it.
    //
    // A MAX, so a row whose own value is longer still gets its full width
    // — the kind takes max(basis, own) as its floor.
    [[nodiscard]] int value_basis() const noexcept;

private:
    Config cfg_;

    // The body is MEASURED before it is rendered, so only the rows inside the
    // viewport are ever turned into Elements.
    //
    // The panel backs the thread list, which is thousands of rows behind a
    // fourteen-row window. Building every row's Element and letting the scroll
    // viewport clip the other 4986 cost ~82ms a frame at 5k rows — twelve fps
    // on a list whose whole job is to be flicked through. Both halves of that
    // are per-ROW work (constructing the styled runs, then laying out N flex
    // children), and neither is needed for a row that cannot be seen.
    //
    // So: a measure pass that counts LINES without building anything, then a
    // render pass over the intersecting rows only. Content height is preserved
    // exactly by two zero-child spacers standing in for the skipped rows above
    // and below, which keeps the renderer's own max_y writeback — and so the
    // scrollbar thumb and the wheel hit-testing — honest.
    struct Body {
        // offsets[i] = absolute first line of row i; offsets.back() = total.
        // Always non-empty (a single 0 for an empty body), so offsets[ra] is
        // safe for any ra a window computation can produce.
        std::vector<int> offsets{0};
        int total       = 0;
        int cursor_line = 0;
        int cursor_span = 1;
        int selected    = -1;   // resolved: in range, and never a header

        // True when the lines came from Config::items — caller-supplied
        // Elements that the panel did not build. They are measured (so the
        // scroll arithmetic is honest) but never WINDOWED: the panel cannot
        // re-render half of somebody else's Element, and callers with huge
        // item lists already hand it only the visible slice.
        bool opaque     = false;

        // True when measurement stopped early at the height cap, so `total`
        // is a floor rather than the real content height. The panel must not
        // publish max_y from a floor — the renderer's writeback owns it there.
        bool capped     = false;
    };

    [[nodiscard]] Body measure_body() const;

    // Half-open row range [first, last) covering absolute lines [y, y + vh).
    [[nodiscard]] static std::pair<int, int> visible_rows(const Body& b, int y,
                                                          int vh);

    // Elements for rows [first, last) — the ONLY place rows become Elements.
    [[nodiscard]] std::vector<Element> render_range(const Body& b, int first,
                                                    int last) const;

    // Painted line count of one row / one open menu, WITHOUT building it.
    // These must agree with render_item / render_menu exactly: measure decides
    // where the scroll sits and render decides what is under it, so a drift of
    // one line is a row that scrolls off its own help text. panel_test walks a
    // matrix of row shapes asserting render_item(r).size() == item_lines(r).
    [[nodiscard]] int item_lines(const Item& r, int index, bool on_row) const;
    [[nodiscard]] static int menu_lines(const Menu& m);

    // A zero-child box that occupies `n` rows: the skipped rows' geometry
    // without their cost.
    [[nodiscard]] static Element spacer_rows(int n);

    [[nodiscard]] std::vector<Element> render_item(const Item& r, int index) const;
    [[nodiscard]] std::vector<Element> render_menu(const Menu& m) const;
    [[nodiscard]] std::pair<std::string, Style> render_control(const Item& r,
                                                               int index) const;
    // Same, reporting where the painted caret glyph landed (byte offset
    // into the returned string; npos = none) so the item renderer can
    // anchor the hardware cursor on that cell.
    [[nodiscard]] std::pair<std::string, Style> render_control(
        const Item& r, int index, std::size_t* caret_at) const;
    // Same again, also collecting the control's OWN styled runs when it has
    // more than one hue to express — a meter's filled head against its
    // unfilled track. Empty when the kind styles itself uniformly, which is
    // every text kind.
    [[nodiscard]] std::pair<std::string, Style> render_control(
        const Item& r, int index, std::size_t* caret_at,
        std::vector<StyledRun>* runs_at) const;

    // The shared row idiom: a leading cell that grows, a gap, a trailing cell.
    // FLEX — the layout owns the width, so there is nothing to get wrong.
    [[nodiscard]] static Element row_line(Element lead, Element trail,
                                          bool trailing_secondary,
                                          Style gap_style,
                                          bool value_primary = false);
    [[nodiscard]] static Element right_line(Element content);
};
} // namespace maya
