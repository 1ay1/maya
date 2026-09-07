#pragma once
// maya::panel::Theme — the panel family's palette, one place.
//
// ANSI-relative, never hex. A widget that hardcodes #181825 looks correct on
// exactly one terminal theme and wrong everywhere else. The panel paints NO
// background; it inherits the terminal's, and only the cursor row is tinted.

#include "../../style/color.hpp"

namespace maya::panel {

struct Theme {
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

} // namespace maya::panel

namespace maya {
// Compatibility spelling — the type predates the panel/ folder.
using PanelTheme = panel::Theme;
} // namespace maya
