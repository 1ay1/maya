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

#include <atomic>
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

    /// Resolve a slot-kind Color against this theme. Literals pass through
    /// untouched, so a host override always wins over a widget default.
    [[nodiscard]] constexpr Color resolve(Color c) const noexcept {
        if (c.kind() != Color::Kind::Slot) return c;
        switch (c.theme_slot()) {
            case ThemeSlot::Primary:     return primary;
            case ThemeSlot::Secondary:   return secondary;
            case ThemeSlot::Accent:      return accent;
            case ThemeSlot::Success:     return success;
            case ThemeSlot::Error:       return error;
            case ThemeSlot::Warning:     return warning;
            case ThemeSlot::Info:        return info;
            case ThemeSlot::Text:        return text;
            case ThemeSlot::InverseText: return inverse_text;
            case ThemeSlot::Muted:       return muted;
            case ThemeSlot::Surface:     return surface;
            case ThemeSlot::Background:  return background;
            case ThemeSlot::Border:      return border;
            case ThemeSlot::DiffAdded:   return diff_added;
            case ThemeSlot::DiffRemoved: return diff_removed;
            case ThemeSlot::DiffChanged: return diff_changed;
            case ThemeSlot::Highlight:   return highlight;
            case ThemeSlot::Selection:   return selection;
            case ThemeSlot::Cursor:      return cursor;
            case ThemeSlot::Link:        return link;
            case ThemeSlot::Placeholder: return placeholder;
            case ThemeSlot::Shadow:      return shadow;
            case ThemeSlot::Overlay:     return overlay;
        }
        return text;
    }
};

namespace theme {

// The theme in force, readable from anywhere that paints.
//
// Slot-kind colours are resolved against this at SGR-emit time, which is the
// only moment a widget Config default (written at static-init, with no theme
// in scope) can become a real hue. It is a POINTER into a table of themes
// with static storage, so a read is one relaxed atomic load rather than a
// copy of 24 Colors per span.
//
// The runtime re-seats it on every theme swap; before that it is `native`,
// which resolves every slot to the terminal's own palette — the safe answer
// for a unit test or a one-shot print that never starts a runtime.
namespace detail {
inline std::atomic<const Theme*>& live_slot() noexcept;
}

[[nodiscard]] inline const Theme& live() noexcept;
inline void set_live(const Theme& t) noexcept;

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
// Does this theme own the canvas?
// ============================================================================
//
// The one question that separates `native` from every RGB scheme, and it is
// answered by DATA rather than by a name or a table index: a theme owns the
// canvas exactly when it states a real background.
//
// `native` sets `.background = default_color()`, meaning "whatever the user's
// terminal already is". There is nothing to paint, and painting anything
// would be a guess — the bug native exists to avoid. So the app fills
// nothing and the terminal shows through, including its own transparency
// and background image.
//
// A scheme like Dracula states `#282A36`. Half-applying it — Dracula's
// foregrounds over the user's own background — is what makes a "theme"
// feel broken: the hues were contrast-checked against THAT canvas, not
// against whatever is behind them. So a theme that names a background
// means it, and the host fills the frame with it.
//
// Hosts use this to decide whether to wrap their root element in a bgc():
//
//     Element root = build();
//     if (theme::owns_canvas(t)) root = std::move(root) | bgc(t.background);
//
[[nodiscard]] constexpr bool owns_canvas(const Theme& t) noexcept {
    return t.background.kind() != Color::Kind::Default;
}

// ── Categorical series colour ───────────────────────────────────────
//
// The hue for the i-th slice of a chart whose slices are NAMED THINGS
// rather than states — tools by call count, providers by spend, files by
// churn.
//
// This is a genuinely different question from "what colour is an error",
// and conflating the two is why hosts kept growing private RGB ramps: a
// status palette says green=good / red=bad, which is a lie when the slices
// are "read" and "write". But a private ramp is a palette the theme cannot
// see, so it stops following the user the moment they pick a scheme.
//
// Both concerns are satisfied by deriving the ramp FROM the theme's own
// role slots, in an order that alternates across the spectrum so adjacent
// slices contrast instead of walking through two neighbouring blues. The
// slots reused here (link/primary/warning/success/accent/info/error/
// secondary) are the eight the schemes keep maximally distinct, because
// they are the ones that must stay distinguishable as UI roles.
//
// `i` wraps, so a caller never has to bound it.
[[nodiscard]] constexpr Color series(const Theme& t, std::size_t i) noexcept {
    const Color ramp[] = {
        t.link, t.primary, t.warning, t.success,
        t.accent, t.info, t.error, t.secondary,
    };
    return ramp[i % (sizeof(ramp) / sizeof(ramp[0]))];
}

// The "none of the above" slice. A catch-all is the ABSENCE of a category,
// so giving it a category's colour makes it read as one more of them.
[[nodiscard]] constexpr Color series_other(const Theme& t) noexcept {
    return t.muted;
}

// ── live theme slot (declared above; defined here, now that native exists)
namespace detail {
inline std::atomic<const Theme*>& live_slot() noexcept {
    static std::atomic<const Theme*> t{&native};
    return t;
}
}  // namespace detail

[[nodiscard]] inline const Theme& live() noexcept {
    return *detail::live_slot().load(std::memory_order_relaxed);
}

inline void set_live(const Theme& t) noexcept {
    detail::live_slot().store(&t, std::memory_order_relaxed);
}

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

[[nodiscard]] inline bool starts_with(std::string_view h, std::string_view n) {
    return h.size() >= n.size() && h.substr(0, n.size()) == n;
}

// Terminals that ARE truecolor but only say so via COLORTERM — which ssh
// does not forward (it is not in most sshd AcceptEnv lists) while TERM is.
// Over ssh the escape bytes are interpreted by the LOCAL terminal, so when
// TERM names one of these, emitting 38;2 is always right. Without this a
// remote session silently drops to 16 colours and vivid RGB row bands
// quantise into garish solid blocks.
inline constexpr std::string_view kTrueColorTerms[] = {
    "kitty", "ghostty", "wezterm", "alacritty", "foot",
    "iterm",  "konsole", "contour", "rio",
};

// Host applications that set their own marker instead of COLORTERM.
// Checked only as a LAST resort before the TERM heuristics, because a
// program name is weaker evidence than a capability claim.
[[nodiscard]] inline bool truecolor_host() {
    // Windows Terminal and ConEmu both do 24-bit and neither reliably
    // sets COLORTERM.
    if (env_set("WT_SESSION")) return true;
    if (env_or("ConEmuANSI") == "ON") return true;
    const std::string_view tp = env_or("TERM_PROGRAM");
    // Apple Terminal is deliberately ABSENT: it is 256-colour only, and
    // claiming truecolor there produces visibly wrong hues.
    return tp == "iTerm.app" || tp == "WezTerm" || tp == "ghostty"
        || tp == "vscode"    || tp == "Hyper"   || tp == "rio";
}

}  // namespace detail

// Detect what the terminal can render.
//
//   `tty` is whether stdout is a terminal; a redirect gets Mono so a piped
//   log is plain text rather than escape soup. The caller supplies it
//   because maya's platform layer owns that question.
[[nodiscard]] inline ColorTier detect_tier(bool tty) {
    using namespace detail;

    // ── 0. Explicit override ────────────────────────────────────────
    // Above even NO_COLOR, because it is the escape hatch for the case
    // detection cannot win: truecolor passthrough inside tmux, a terminal
    // nobody has heard of, or a test that needs a fixed answer.
    const std::string_view forced_tier = env_or("MAYA_COLOR");
    if (forced_tier == "truecolor" || forced_tier == "24bit"
        || forced_tier == "3")                    return ColorTier::TrueColor;
    if (forced_tier == "256" || forced_tier == "2") return ColorTier::Ansi256;
    if (forced_tier == "16" || forced_tier == "basic"
        || forced_tier == "1")                    return ColorTier::Ansi16;
    if (forced_tier == "none" || forced_tier == "mono"
        || forced_tier == "0")                    return ColorTier::Mono;
    // "auto" or anything unrecognised falls through to detection.

    // ── 1. NO_COLOR ─────────────────────────────────────────────
    // Any non-empty value disables colour (no-color.org). An EMPTY value is
    // treated as unset, which the standard is explicit about — `NO_COLOR= cmd`
    // must still be colourful. env_set() already encodes that.
    if (env_set("NO_COLOR")) return ColorTier::Mono;

    // ── 2. CLICOLOR_FORCE / the tty test ──────────────────────────────
    // A redirect gets Mono so a piped log is text, not escape soup;
    // CLICOLOR_FORCE exists precisely to override that for CI and pagers.
    const bool forced = env_set("CLICOLOR_FORCE")
                        && env_or("CLICOLOR_FORCE") != "0";

    const std::string_view term = env_or("TERM");
    if (term == "dumb") return ColorTier::Mono;
    if (!tty && !forced) return ColorTier::Mono;

    // ── 3. COLORTERM ── the terminal's own capability claim ───────────────
    const std::string_view ct = env_or("COLORTERM");
    if (ct == "truecolor" || ct == "24bit") return ColorTier::TrueColor;

    // ── 4. TERM conventions, most specific first ───────────────────────
    if (contains(term, "direct")) return ColorTier::TrueColor;

    // Named truecolor terminals. This is the ssh case: COLORTERM does not
    // survive the hop, TERM does, and the bytes are drawn by the local
    // terminal either way.
    for (const std::string_view name : kTrueColorTerms)
        if (contains(term, name)) return ColorTier::TrueColor;

    // ── 5. Host-application markers ─────────────────────────────────
    // Weaker evidence than a capability claim, so it sits below both.
    if (truecolor_host()) return ColorTier::TrueColor;

    if (contains(term, "256color")) return ColorTier::Ansi256;
    if (term.empty())              return ColorTier::Mono;

    // ── 6. The xterm/screen/tmux family ───────────────────────────────
    // Real xterm has done 256 colours for two decades, and screen and tmux
    // both pass 38;5 through. Calling these Ansi16 was the harmful default:
    // indexed SGR is universally safe here, and quantising to sixteen makes
    // tuned greys and dark tints collapse into primaries.
    if (starts_with(term, "xterm") || starts_with(term, "screen")
        || starts_with(term, "tmux")  || starts_with(term, "rxvt")
        || starts_with(term, "vte")   || starts_with(term, "linux"))
        return ColorTier::Ansi256;

    // 7. Anything else that looks like a terminal gets the sixteen colours
    //    every terminal has had since the VT100's descendants.
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
