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
#include "scrollbar.hpp"

namespace maya {

// ============================================================================
//  Theme
// ============================================================================
//
// ANSI-relative, never hex. A widget that hardcodes #181825 looks correct on
// exactly one terminal theme and wrong everywhere else. The panel paints NO
// background; it inherits the terminal's, and only the cursor row is tinted.
struct PanelTheme {
    Color title      = Color::bright_white();
    Color label      = Color::bright_white();
    Color help       = Color::bright_black();
    Color value      = Color::cyan();
    Color value_edit = Color::blue();
    Color on         = Color::green();
    Color off        = Color::bright_black();
    Color origin     = Color::bright_black();
    Color locked     = Color::bright_black();
    Color error      = Color::red();
    Color good       = Color::green();
    Color busy       = Color::yellow();
    Color cursor     = Color::blue();      // the edge bar
    Color active     = Color::bright_magenta();
    Color match      = Color::cyan();      // fuzzy-match highlight

    // Cursor-row wash. A tint, not a reverse-video slab: ANSI bright-white is
    // a cream/yellow tone in several popular palettes, and a full-width band
    // of it across a wide settings panel reads as a rendering fault.
    //
    // Hex, not an ANSI slot, because this is the ONE place a literal is
    // right: it must sit a hair above the terminal's background on both dark
    // and light themes, and every ANSI slot is either invisible against one
    // of them or loud against the other. Verified by asserting the emitted
    // SGR code, which is what caught an earlier "invisible black".
    Color row_bg     = Color::hex(0x232634);
};

// ============================================================================
//  Controls — what a row's trailing cell can BE
// ============================================================================
namespace panel {

// Plain text. A picker row's trailing cell is this.
struct Label {
    std::string text;
    Style       style{};
};

struct Toggle { bool on = false; };

// Pick one of a small closed set. The option list travels separately (see
// Config::menu) and only while it is open.
struct Choice { std::string label; };

// A value owned by ANOTHER overlay: Enter hands off to a real picker rather
// than opening an inline list.
struct Pick {
    std::string label;
    std::string placeholder = "\xe2\x80\x94";
};

struct Number { std::int64_t value = 0; };

struct Slider {
    double value = 0.0, min = 0.0, max = 1.0;
    int    decimals = 2;
};

struct Text {
    std::string value;
    std::size_t caret = std::string::npos;   // npos ⇒ not being edited
    std::string placeholder;
};

// No plaintext — by construction. `filled` is a character count, so no render
// path, present or future, can leak a credential to the screen.
struct Secret {
    std::size_t filled = 0;
    std::size_t caret  = std::string::npos;
};

struct Path {
    std::string value;
    std::size_t caret = std::string::npos;
    enum class State : std::uint8_t { Unknown, Exists, Missing };
    State       state = State::Unknown;
    std::string placeholder;
};

// A row that DOES something and reports its outcome where the action lives.
struct Action {
    enum class Tone : std::uint8_t { Neutral, Busy, Good, Bad };
    std::string status;
    std::string hint = "press Enter";
    Tone        tone = Tone::Neutral;
};

} // namespace panel

using PanelControl = std::variant<panel::Label, panel::Toggle, panel::Choice,
                                  panel::Pick, panel::Number, panel::Slider,
                                  panel::Text, panel::Secret, panel::Path,
                                  panel::Action>;

class Panel {
public:
    // ── The one row type ─────────────────────────────────────────────────
    struct Row {
        // Column 0-1: a status glyph (● active, ⚠ pending-delete, tree elbow).
        std::string badge;
        Style       badge_style{};

        std::string leading;
        Style       leading_style{};

        // Optional match-highlight: byte offsets into `leading` to render in
        // the accent hue (bold) — the characters a fuzzy query matched. Empty
        // ⇒ leading is painted as one span. The highlight keeps its hue even
        // on the cursor row, so "which chars matched" stays legible.
        std::vector<int> highlight;
        Color            highlight_fg = Color::bright_cyan();

        // The trailing cell. `Label` covers every picker row; the other
        // alternatives are the editable controls a settings row carries.
        PanelControl control = panel::Label{};

        // Convenience for the common case: a plain trailing string. Setting
        // these is equivalent to `control = Label{text, style}`, and exists so
        // a list row reads as a list row rather than wrapping every string in
        // a variant alternative. `control` wins if both are set.
        std::string trailing;
        Style       trailing_style{};

        // Dim provenance after the value ("default", "env: X", "pinned").
        std::string origin;

        // A one-line description shown ONLY while the cursor is on this row.
        // Painting it under every row doubles a list's height — a 22-setting
        // pane became 50-odd rows in a 14-row viewport.
        std::string help;

        // Non-empty ⇒ invalid; rendered under the row in the error tone.
        std::string error;

        // DERIVED from Config::selected, never set by a caller. It lives on
        // the row only because the renderer needs it per-row; writing it from
        // outside is silently ignored. One owner for "where is the cursor".
        bool selected = false;
        bool active   = false;   // "currently in use" — a persistent marker
        bool locked   = false;   // visible, not editable
        std::string locked_reason;

        // A non-selectable SECTION HEADER: upper-cased, with a rule to the
        // right edge. Deliberately unlike a locked row, which is what made
        // "Endpoint (auto-detected)" read as a section title.
        bool is_header = false;

        // The trailing cell is secondary and yields space FIRST (a command
        // palette's description must never eat its command name). Default:
        // the LEADING cell yields first, so a long label truncates before it
        // can push the value off the row.
        bool trailing_secondary = false;
    };

    // The inline option list, present only while a Choice row is open.
    //
    // Enums only. There is no query field: a set large enough to need
    // searching belongs in its own overlay, reached by a `Pick` row.
    //
    // TWO markers, never conflated — ◉ is the COMMITTED value, ❯ is the
    // CURSOR. They coincide on open and diverge the moment you move.
    struct Menu {
        std::vector<std::string> options;
        std::vector<std::string> hints;
        int  highlighted = 0;
        int  current     = -1;
        int  scroll      = 0;
        int  viewport    = 8;
    };

    struct Config {
        std::string title;        // centred on the top border
        std::string subtitle;     // status line above the body

        std::vector<Row> rows;
        // Index into `rows` (or `items`) of the cursor. <0 = no selection.
        int              selected = -1;

        // Pre-built rows, for callers that own their own row rendering
        // (the thread list's virtualisation). Ignored when `rows` is set.
        std::vector<Element> items;

        std::optional<Menu> menu;
        int                 menu_row = -1;

        std::vector<Element> header;   // above the body, never scrolls
        std::string          note;     // below the body
        std::vector<Element> footer;   // key hints

        // Borrowed; must outlive the built Element. Null disables scrolling.
        ScrollState* scroll     = nullptr;
        int          viewport_h = 14;

        // The HOST clamps this to the terminal: a min-width wider than the
        // screen is not a minimum but an overflow, and the overlay centres the
        // panel so the excess is split off both edges and the labels vanish.
        int   min_width = 60;
        Color accent    = Color::blue();

        // Colour of the edge bar on the ACTIVE row (the persistent "currently
        // in use" marker, distinct from the cursor). The cursor wins on
        // overlap — where you ARE outranks where you were.
        Color active_color = Color::bright_magenta();

        PanelTheme     theme{};
        ScrollbarStyle scrollbar_style = ScrollbarStyle::neon();
    };

    explicit Panel(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const;

    // U+258E LEFT ONE QUARTER BLOCK — the cursor bar in column 0.
    static constexpr const char* kEdgeBar = "\xe2\x96\x8e";

    // Columns an EDITED value may occupy before it scrolls horizontally under
    // its caret. A fixed budget rather than a measured one: the true width is
    // only known inside the flex layout, and asking for it there is what led
    // to the reserve-constant bugs. This is generous enough for endpoints and
    // paths, and the layout still clips whatever exceeds it — the budget only
    // decides WHERE the window sits, never how much space the cell gets.
    static constexpr int kEditBudget = 34;

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
    // These must agree with render_row / render_menu exactly: measure decides
    // where the scroll sits and render decides what is under it, so a drift of
    // one line is a row that scrolls off its own help text. panel_test walks a
    // matrix of row shapes asserting render_row(r).size() == row_lines(r).
    [[nodiscard]] int row_lines(const Row& r, int index, bool on_row) const;
    [[nodiscard]] static int menu_lines(const Menu& m);

    // A zero-child box that occupies `n` rows: the skipped rows' geometry
    // without their cost.
    [[nodiscard]] static Element spacer_rows(int n);

    [[nodiscard]] std::vector<Element> render_row(const Row& r, int index) const;
    [[nodiscard]] std::vector<Element> render_menu(const Menu& m) const;
    [[nodiscard]] std::pair<std::string, Style> render_control(const Row& r,
                                                               int index) const;

    // The shared row idiom: a leading cell that grows, a gap, a trailing cell.
    // FLEX — the layout owns the width, so there is nothing to get wrong.
    [[nodiscard]] static Element row_line(Element lead, Element trail,
                                          bool trailing_secondary,
                                          Style gap_style);
    [[nodiscard]] static Element right_line(Element content);
};
} // namespace maya
