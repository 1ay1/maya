#pragma once
// maya::theme - Compile-time palette definitions and theme system
//
// Every built-in theme is constexpr - fully resolved at compile time.
// Custom themes can be derived from a base with selective overrides,
// also at compile time. The 16-color ANSI variants ensure maya looks
// decent on minimal terminals without truecolor support.

#include <utility>

#include "color.hpp"

namespace maya {

// ============================================================================
// Theme - named color slots for semantic UI coloring
// ============================================================================
// 24 named slots covering general UI chrome, status colors, and diff
// highlighting. Every field has a sensible default so partial overrides
// via derive() are safe.

struct Theme {
    // --- Primary palette ---
    Color primary;
    Color secondary;
    Color accent;

    // --- Status ---
    Color success;
    Color error;
    Color warning;
    Color info;

    // --- Text ---
    Color text;
    Color inverse_text;
    Color muted;

    // --- Surfaces ---
    Color surface;
    Color background;
    Color border;

    // --- Diff ---
    Color diff_added;
    Color diff_removed;
    Color diff_changed;

    // --- Extras ---
    Color highlight;
    Color selection;
    Color cursor;
    Color link;
    Color placeholder;
    Color shadow;
    Color overlay;

    // ========================================================================
    // derive - create a new theme from a base with compile-time overrides
    // ========================================================================
    // Usage:
    //   constexpr auto my_theme = Theme::derive(theme::dark,
    //       [](Theme& t) { t.primary = Color::hex(0x00FF00); });
    //
    // The callable receives a mutable Theme& and patches it in place.
    // Multiple overrides compose left-to-right.

    template <typename... Fns>
        requires (std::invocable<Fns, Theme&> && ...)
    [[nodiscard]] static constexpr Theme derive(const Theme& base, Fns&&... fns) {
        Theme t = base;
        (std::forward<Fns>(fns)(t), ...);
        return t;
    }

    constexpr bool operator==(const Theme&) const = default;
};

// ============================================================================
// Built-in themes
// ============================================================================

namespace theme {

// --------------------------------------------------------------------------
// dark - default truecolor dark theme
// --------------------------------------------------------------------------
// Inspired by modern dark editor themes. Muted backgrounds, vibrant accents.

inline constexpr Theme dark {
    .primary       = Color::hex(0x7AA2F7),   // soft blue
    .secondary     = Color::hex(0x9ECE6A),   // green
    .accent        = Color::hex(0xBB9AF7),   // purple

    .success       = Color::hex(0x9ECE6A),
    .error         = Color::hex(0xF7768E),   // red-pink
    .warning       = Color::hex(0xE0AF68),   // amber
    .info          = Color::hex(0x7DCFFF),   // cyan-ish

    .text          = Color::hex(0xC0CAF5),   // light blue-gray
    .inverse_text  = Color::hex(0x1A1B26),   // near-black
    .muted         = Color::hex(0x565F89),   // dim blue-gray

    .surface       = Color::hex(0x24283B),   // dark blue-gray
    .background    = Color::hex(0x1A1B26),   // near-black
    .border        = Color::hex(0x3B4261),

    .diff_added    = Color::hex(0x283B4D),   // dark teal tint
    .diff_removed  = Color::hex(0x3D2029),   // dark red tint
    .diff_changed  = Color::hex(0x2B3040),

    .highlight     = Color::hex(0x33467C),   // selection-ish blue
    .selection     = Color::hex(0x364A82),
    .cursor        = Color::hex(0xC0CAF5),
    .link          = Color::hex(0x73DACA),   // teal
    .placeholder   = Color::hex(0x444B6A),
    .shadow        = Color::hex(0x0A0B10),
    .overlay       = Color::hex(0x1F2335),
};

// --------------------------------------------------------------------------
// light - truecolor light theme
// --------------------------------------------------------------------------

inline constexpr Theme light {
    .primary       = Color::hex(0x3760BF),
    .secondary     = Color::hex(0x587539),
    .accent        = Color::hex(0x7847BD),

    .success       = Color::hex(0x587539),
    .error         = Color::hex(0xC64343),
    .warning       = Color::hex(0x8C6C3E),
    .info          = Color::hex(0x2E7DE9),

    .text          = Color::hex(0x3760BF),
    .inverse_text  = Color::hex(0xD4D6E4),
    .muted         = Color::hex(0x848CB5),

    .surface       = Color::hex(0xD4D6E4),
    .background    = Color::hex(0xE1E2E7),
    .border        = Color::hex(0xC4C8DA),

    .diff_added    = Color::hex(0xC6E7C6),
    .diff_removed  = Color::hex(0xF0D0D0),
    .diff_changed  = Color::hex(0xD8DCF0),

    .highlight     = Color::hex(0xB6BFEE),
    .selection     = Color::hex(0xC4CCEE),
    .cursor        = Color::hex(0x3760BF),
    .link          = Color::hex(0x387068),
    .placeholder   = Color::hex(0xA1A6C5),
    .shadow        = Color::hex(0xB4B5B9),
    .overlay       = Color::hex(0xD8DAE4),
};

// --------------------------------------------------------------------------
// dark_ansi - 16-color dark theme for minimal terminals
// --------------------------------------------------------------------------

inline constexpr Theme dark_ansi {
    .primary       = Color::bright_blue(),
    .secondary     = Color::green(),
    .accent        = Color::bright_magenta(),

    .success       = Color::green(),
    .error         = Color::bright_red(),
    .warning       = Color::yellow(),
    .info          = Color::bright_cyan(),

    .text          = Color::white(),
    .inverse_text  = Color::black(),
    .muted         = Color::bright_black(),

    .surface       = Color::black(),
    .background    = Color::black(),
    .border        = Color::bright_black(),

    .diff_added    = Color::green(),
    .diff_removed  = Color::red(),
    .diff_changed  = Color::yellow(),

    .highlight     = Color::blue(),
    .selection     = Color::blue(),
    .cursor        = Color::white(),
    .link          = Color::cyan(),
    .placeholder   = Color::bright_black(),
    .shadow        = Color::black(),
    .overlay       = Color::black(),
};

// --------------------------------------------------------------------------
// light_ansi - 16-color light theme for minimal terminals
// --------------------------------------------------------------------------

inline constexpr Theme light_ansi {
    .primary       = Color::blue(),
    .secondary     = Color::green(),
    .accent        = Color::magenta(),

    .success       = Color::green(),
    .error         = Color::red(),
    .warning       = Color::yellow(),
    .info          = Color::cyan(),

    .text          = Color::black(),
    .inverse_text  = Color::bright_white(),
    .muted         = Color::bright_black(),

    .surface       = Color::bright_white(),
    .background    = Color::bright_white(),
    .border        = Color::white(),

    .diff_added    = Color::green(),
    .diff_removed  = Color::red(),
    .diff_changed  = Color::yellow(),

    .highlight     = Color::bright_blue(),
    .selection     = Color::bright_cyan(),
    .cursor        = Color::black(),
    .link          = Color::cyan(),
    .placeholder   = Color::white(),
    .shadow        = Color::white(),
    .overlay       = Color::bright_white(),
};

} // namespace theme

// Compile-time validation
static_assert(theme::dark.primary == Color::hex(0x7AA2F7));
static_assert(theme::dark_ansi.text == Color::bright_white() || theme::dark_ansi.text == Color::white());
static_assert(theme::dark == Theme::derive(theme::dark, [](Theme&) {}));
static_assert(Theme::derive(theme::dark, [](Theme& t) {
    t.primary = Color::red();
}).primary == Color::red());

} // namespace maya
