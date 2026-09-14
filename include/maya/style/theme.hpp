#pragma once
// maya::theme — semantic color slots, and the terminal they land on.
//
// ── The default is NATIVE ────────────────────────────────────────────────
//
// theme::native states almost no colors of its own. Its slots resolve to
// Color::default_color() — SGR 39/49, "whatever this terminal uses" — and
// to the sixteen NAMED ansi slots, which are indices into the palette the
// user configured, not colors maya picked.
//
// That is the only theme that is correct everywhere, and the reason is
// polarity. A palette of literal RGB is authored against one background:
// pick #565F89 for muted text and it reads on black and vanishes on light
// grey. maya cannot know the background — no terminal reliably reports it
// — so the only safe move is not to state one. Emit `39` and the user's
// foreground arrives, whatever they chose; emit `31` and their red
// arrives, already contrast-checked against their own background by them.
//
// The cost is that native cannot express a shade the palette lacks. Where
// a theme would use a dim blue-grey, native uses BOLD and DIM attributes
// for hierarchy instead of a third colour — which is what a 16-colour
// terminal has always done, and what degrades cleanly to monochrome.
//
// ── The named schemes are opt-in ─────────────────────────────────────────
//
// style/schemes.hpp carries Dracula, Nord, Gruvbox, Solarized, Catppuccin,
// TokyoNight and the rest, generated from mbadolato/iTerm2-Color-Schemes.
// Those DO state literal colors, including a background, which is the
// point of choosing one: the user has said "paint it like this" and maya
// then owns the whole surface. They are never a default.

#include <cstdlib>
#include <cstring>
#include <string_view>
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
    //   constexpr auto my_theme = Theme::derive(theme::native,
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

namespace theme {

// ============================================================================
// native — the default. The terminal's own colors, and nothing else.
// ============================================================================
//
// Every slot here is Default (SGR 39/49) or Named (SGR 30-37/90-97). Not one
// is an RGB literal, which is what makes it correct on a light background, a
// dark one, a 16-color tty and a monochrome one alike.
//
// Reading the slots:
//
//   text/background  Default. The user's own pair, already legible together
//                    because they chose it. Stating either is how a TUI ends
//                    up as grey-on-grey in someone's light terminal.
//
//   error/success/   The palette's red/green/yellow/cyan. Every program
//   warning/info     emitting ANSI has meant these for forty years, so the
//                    user's palette has already contrast-checked them.
//
//   muted            bright_black — the dim tier every palette carries. This
//                    is the one slot with real tension: on some light schemes
//                    bright_black is close to the background. Prefer the DIM
//                    attribute over this slot where the hierarchy matters
//                    more than the hue.
//
//   surface/overlay  Default, NOT a shade. maya cannot darken a background it
//                    has not been told, and a guessed surface is the exact
//                    bug this theme exists to avoid. A panel separates itself
//                    with a border, not a fill.
inline constexpr Theme native {
    .primary       = Color::bright_blue(),
    .secondary     = Color::bright_green(),
    .accent        = Color::bright_magenta(),

    .success       = Color::green(),
    .error         = Color::red(),
    .warning       = Color::yellow(),
    .info          = Color::cyan(),

    .text          = Color::default_color(),
    .inverse_text  = Color::default_color(),
    .muted         = Color::bright_black(),

    .surface       = Color::default_color(),
    .background    = Color::default_color(),
    .border        = Color::bright_black(),

    // A diff tint is a BACKGROUND wash, and native has no background to
    // wash. The named green/red carry it as foreground instead, which is
    // how diff has always read on a 16-color terminal.
    .diff_added    = Color::green(),
    .diff_removed  = Color::red(),
    .diff_changed  = Color::yellow(),

    .highlight     = Color::bright_blue(),
    .selection     = Color::blue(),
    .cursor        = Color::default_color(),
    .link          = Color::bright_cyan(),
    .placeholder   = Color::bright_black(),
    .shadow        = Color::default_color(),
    .overlay       = Color::default_color(),
};

// ============================================================================
// Capability detection
// ============================================================================
//
// What a terminal can RENDER, decided before the first colored byte. The
// order is the one every mature TUI converges on, and each step is a signal
// the user or their terminal actually set — never a guess from TERM alone.

enum class ColorTier : std::uint8_t {
    Mono,        // no color at all
    Ansi16,      // 30-37 / 90-97
    Ansi256,     // 38;5;n
    TrueColor,   // 38;2;r;g;b
};

namespace detail {

[[nodiscard]] inline bool env_set(const char* k) {
    const char* v = std::getenv(k);
    return v != nullptr && *v != '\0';
}

[[nodiscard]] inline std::string_view env_or(const char* k, std::string_view d = {}) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string_view{v} : d;
}

[[nodiscard]] inline bool contains(std::string_view h, std::string_view n) {
    return h.find(n) != std::string_view::npos;
}

}  // namespace detail

// Detect what the terminal can render.
//
//   `tty` is whether stdout is a terminal; a redirect gets Mono so a piped
//   log is plain text rather than escape soup. The caller supplies it
//   because maya's platform layer owns that question.
[[nodiscard]] inline ColorTier detect_tier(bool tty) {
    using namespace detail;

    // 1. NO_COLOR, at any value, wins over everything. (no-color.org)
    if (env_set("NO_COLOR")) return ColorTier::Mono;

    // 2. CLICOLOR_FORCE overrides the tty test — that is its whole purpose,
    //    so a CI log or a pager can keep color.
    const bool forced = env_set("CLICOLOR_FORCE")
                        && env_or("CLICOLOR_FORCE") != "0";

    const std::string_view term = env_or("TERM");
    if (term == "dumb") return ColorTier::Mono;
    if (!tty && !forced) return ColorTier::Mono;

    // 3. COLORTERM is the truecolor signal, set by the terminal itself.
    const std::string_view ct = env_or("COLORTERM");
    if (ct == "truecolor" || ct == "24bit") return ColorTier::TrueColor;

    // 4. TERM conventions, most specific first.
    if (contains(term, "direct"))    return ColorTier::TrueColor;
    if (contains(term, "256color"))  return ColorTier::Ansi256;
    if (term.empty())                return ColorTier::Mono;

    // 5. Anything that looks like a terminal gets the sixteen colors every
    //    terminal has had since the VT100's descendants.
    return ColorTier::Ansi16;
}

// ============================================================================
// Background polarity
// ============================================================================
//
// Whether the terminal is light or dark. This is ONLY needed to pick a
// default named scheme — theme::native does not care, which is the point of
// it. Detection is deliberately conservative: it reports Unknown rather than
// guess, because a wrong guess here is the bug that makes a TUI unreadable.

enum class Polarity : std::uint8_t { Unknown, Dark, Light };

// COLORFGBG is set by rxvt, urxvt, konsole and a few others: "fg;bg" or
// "fg;default;bg", where the background field is an ANSI index. 0-6 and 8
// are dark; 7 and 15 are the light greys.
//
// This is the cheap signal and it never blocks. The authoritative one is an
// OSC 11 query, which costs a round trip and raw mode — maya does not do it
// here because a library must not steal the terminal mid-render; a host that
// wants it should query at startup and pass the answer to set_polarity().
[[nodiscard]] inline Polarity detect_polarity() {
    const std::string_view v = detail::env_or("COLORFGBG");
    if (v.empty()) return Polarity::Unknown;
    const auto last = v.find_last_of(';');
    if (last == std::string_view::npos) return Polarity::Unknown;
    const std::string_view bg = v.substr(last + 1);
    if (bg.empty()) return Polarity::Unknown;
    // A single index. Anything non-numeric ("default") tells us nothing.
    int n = 0;
    for (const char c : bg) {
        if (c < '0' || c > '9') return Polarity::Unknown;
        n = n * 10 + (c - '0');
    }
    return (n == 7 || n == 15) ? Polarity::Light : Polarity::Dark;
}

}  // namespace theme
}  // namespace maya
