#pragma once
// maya::panel::Row — the ONE row type of the panel family.
//
// What used to distinguish a picker row from a form row is per-cell
// decoration, not structure:
//
//     edge · badge · leading [· highlight] · gap · trailing [· origin]
//
// A picker row fills `leading`/`trailing`; a form row additionally sets
// `control` and `origin`. Nothing about the frame, the scroll, the selection
// or the width arithmetic differs, because there is only one of each.

#include <string>
#include <vector>

#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "control.hpp"

namespace maya::panel {

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
    Control control = Label{};

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

} // namespace maya::panel
