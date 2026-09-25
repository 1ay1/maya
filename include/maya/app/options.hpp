#pragma once
// maya/app/options.hpp — how to take the terminal: Mode, RenderBackend, Options.

#include <string_view>

#include "../style/theme.hpp"

namespace maya {

// ============================================================================
// Mode — rendering mode selection
// ============================================================================

enum class Mode {
    Inline,      // Raw mode, no alt screen, scrollback preserved (Claude Code style)
    Fullscreen,  // Alt screen buffer, double-buffered cell diff
};

// How a rendered frame reaches the host.
//   Ansi — serialize the cell diff to ANSI escapes for a terminal (default).
//   Grid — emit the cell diff as a binary grid frame for a COOPERATING HOST
//          (an Emacs module, a GPU frontend) that paints cells directly,
//          skipping the ANSI encode+reparse round trip.  See render/grid_emit.
enum class RenderBackend {
    Ansi,
    Grid,
};

// ============================================================================
// Options — application configuration
// ============================================================================
// C++20 aggregate. Add new fields with defaults — all existing call sites
// continue to compile unchanged.

/// How to take the terminal: shared by Screen::open and maya::run.
struct Options {
    std::string_view title      = "";               ///< Terminal window title (OSC 0)
    int              fps        = 0;                ///< Continuous rendering at N fps (0 = event-driven)
    bool             mouse      = false;            ///< Enable mouse event reporting
    bool             hover_motion = false;          ///< Also report bare (no-button) motion (mode 1003)
                                                    ///< for hover highlights. Off by default: 1003 floods
                                                    ///< move events and some terminals handle it oddly.
    Mode             mode       = Mode::Fullscreen; ///< Rendering mode
    RenderBackend    backend    = RenderBackend::Ansi; ///< Frame transport (see RenderBackend)
    Theme            theme      = theme::native;      ///< Colour theme
    /// Negotiate the KITTY KEYBOARD PROTOCOL (progressive enhancement,
    /// flag 1 "disambiguate escape codes"). When the terminal supports it,
    /// modifier chords that legacy encoding cannot express — Ctrl+/, Ctrl+Tab,
    /// Shift+Enter, and the Esc-vs-escape-sequence ambiguity — arrive as
    /// unambiguous CSI-u events the parser already decodes. Terminals that
    /// don't understand it ignore the `\x1b[>1u` push (it's a private CSI),
    /// so this is safe to leave on; disable via env MAYA_NO_KITTY_KEYBOARD=1
    /// or by setting this false.
    bool             enhanced_keyboard = true;
};

} // namespace maya
