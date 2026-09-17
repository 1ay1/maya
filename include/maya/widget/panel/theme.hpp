#pragma once
// maya::panel::Theme — the panel family's palette, one place.
//
// ANSI-relative, never hex. A widget that hardcodes #181825 looks correct on
// exactly one terminal theme and wrong everywhere else. The panel paints NO
// background; it inherits the terminal's, and only the cursor row is tinted.

#include "../../style/color.hpp"

namespace maya::panel {

struct Theme {
    Themed title      = ThemeSlot::Text;
    Themed label      = ThemeSlot::Text;
    Themed help       = ThemeSlot::Muted;
    Themed value      = ThemeSlot::Info;
    Themed value_edit = ThemeSlot::Primary;
    Themed on         = ThemeSlot::Success;
    Themed off        = ThemeSlot::Muted;
    Themed origin     = ThemeSlot::Muted;
    Themed locked     = ThemeSlot::Muted;
    Themed error      = ThemeSlot::Error;
    Themed good       = ThemeSlot::Success;
    Themed busy       = ThemeSlot::Warning;
    Themed cursor     = ThemeSlot::Primary;      // the edge bar
    Themed active     = ThemeSlot::Accent;
    Themed match      = ThemeSlot::Info;      // fuzzy-match highlight

    // Cursor-row wash. A tint, not a reverse-video slab: ANSI bright-white is
    // a cream/yellow tone in several popular palettes, and a full-width band
    // of it across a wide settings panel reads as a rendering fault.
    //
    // Hex, not an ANSI slot, because this is the ONE place a literal is
    // right: it must sit a hair above the terminal's background on both dark
    // and light themes, and every ANSI slot is either invisible against one
    // of them or loud against the other. Verified by asserting the emitted
    // SGR code, which is what caught an earlier "invisible black".
    Themed row_bg     = ThemeSlot::Surface;
};

} // namespace maya::panel

namespace maya {
// Compatibility spelling — the type predates the panel/ folder.
using PanelTheme = panel::Theme;
} // namespace maya
