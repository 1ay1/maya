#pragma once
// maya::panel::ItemCtx — what an item widget may know while rendering.
//
// Deliberately tiny. An item renders from its own value plus this context,
// and NOTHING else — no Config, no row list, no layout. That is what keeps
// "one item kind = one widget file" honest: a widget that could reach the
// whole panel would grow panel logic.

#include "theme.hpp"

namespace maya::panel {

struct ItemCtx {
    const Theme& theme;

    // Choice only: is this row's dropdown currently open?
    bool open = false;

    // Text/Path: columns the edited value may occupy before it scrolls
    // horizontally under its caret (Panel::edit_budget()). 0 = unmeasured.
    int edit_budget = 0;

    // OUT: byte offset of the painted caret glyph in the returned string
    // (npos = no live caret). Written by the editable kinds; the panel
    // uses it to anchor the HARDWARE cursor on that cell — terminal-side
    // blink, IME composition at the right spot, screen readers. Null when
    // the host didn't ask.
    std::size_t* caret_out = nullptr;
};

} // namespace maya::panel
