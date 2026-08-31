#pragma once
// maya::style::style - Compile-time style builder for text visual properties
//
// Style is a value type holding all text decoration and color information.
// Fully constexpr constructible, trivially copyable, and composable via a
// fluent builder pattern or the merge operator|. Generates ANSI SGR escape
// sequences for terminal rendering.

#include "color.hpp"

#include <optional>
#include <string>

namespace maya {

// ============================================================================
// Style - Immutable, composable text style descriptor
// ============================================================================
// Usage:
//   constexpr auto heading = Style{}.with_bold().with_fg(Color::cyan());
//   constexpr auto warning = bold_style | Style{}.with_fg(Color::yellow());
//   std::string sgr = heading.to_sgr();   // "\x1b[1;36m"

struct Style {
    // -- Visual properties ---------------------------------------------------

    /// Foreground color. nullopt means "inherit / terminal default".
    std::optional<Color> fg{};

    /// Background color. nullopt means "inherit / terminal default".
    std::optional<Color> bg{};

    bool bold          = false;
    bool dim           = false;
    bool italic        = false;
    bool underline     = false;
    bool strikethrough = false;
    bool inverse       = false;

    /// SGR 8 (conceal / hidden). The cell still occupies its column(s) but
    /// paints NO glyph, independent of the terminal background. This is the
    /// true "occupy width, show nothing" primitive the streaming reveal's
    /// ghost band needs: it lets the not-yet-typed tail keep the REAL text
    /// in the leaf (so word-wrap geometry is byte-for-byte identical to the
    /// settled frame) while rendering invisibly. Substituting spaces
    /// instead — the old ghost path — made the wrapper trim/skip the ghost
    /// run at a wrap boundary, so a wrapping line revealed a WHOLE WORD at a
    /// time (the "starts smooth then bursts" stutter).
    bool conceal       = false;

    /// Hardware-caret anchor (META-attribute — never emitted as SGR).
    /// A widget that wants the REAL terminal cursor at its caret cell
    /// styles that one cell with caret_anchor; the renderer records the
    /// cell's absolute canvas position into Canvas::set_cursor_hint at
    /// paint time, and the inline serializer moves + shows the hardware
    /// cursor there in the frame epilogue. Marking travels in the STYLE
    /// so it survives every paint path uniformly — run splitting, word
    /// wrap, truncation, and the component cache's packed-cell blit
    /// (the bit lives in the interned style, and blitted cells carry
    /// their style_id) — without any widget→layout coordinate plumbing.
    /// Typically combined with with_conceal(): the anchor cell keeps
    /// real glyph bytes for stable wrap geometry but paints nothing,
    /// letting the terminal's own cursor render on top.
    /// Participates in hashing/equality (two styles differing only in
    /// caret_anchor must not alias in the StylePool) but build_sgr
    /// ignores it — zero bytes on the wire.
    bool caret_anchor  = false;

    /// DECSCUSR cursor shape for the hardware caret (META — never SGR).
    /// Only meaningful on a caret_anchor style; the inline serializer
    /// emits `CSI <n> SP q` when it shows the cursor at this cell and
    /// the shape differs from what is already on the wire. 0 = leave
    /// the terminal's configured shape alone. Codes per DECSCUSR:
    /// 1/2 block (blink/steady), 3/4 underline, 5/6 bar. The anchor
    /// style's `fg` doubles as the caret COLOR (OSC 12; emitted only
    /// for Rgb colors, reset via OSC 112 at finalize) — the glyph is
    /// concealed anyway, so fg is free to carry it.
    uint8_t caret_shape = 0;

    // -- Fluent builder (each returns a new Style) ---------------------------

    /// Return a copy with the foreground color set.
    [[nodiscard]] constexpr Style with_fg(Color c) const noexcept {
        Style s = *this;
        s.fg = c;
        return s;
    }

    /// Return a copy with the background color set.
    [[nodiscard]] constexpr Style with_bg(Color c) const noexcept {
        Style s = *this;
        s.bg = c;
        return s;
    }

    /// Return a copy with bold enabled.
    [[nodiscard]] constexpr Style with_bold(bool v = true) const noexcept {
        Style s = *this;
        s.bold = v;
        return s;
    }

    /// Return a copy with dim / faint enabled.
    [[nodiscard]] constexpr Style with_dim(bool v = true) const noexcept {
        Style s = *this;
        s.dim = v;
        return s;
    }

    /// Return a copy with italic enabled.
    [[nodiscard]] constexpr Style with_italic(bool v = true) const noexcept {
        Style s = *this;
        s.italic = v;
        return s;
    }

    /// Return a copy with underline enabled.
    [[nodiscard]] constexpr Style with_underline(bool v = true) const noexcept {
        Style s = *this;
        s.underline = v;
        return s;
    }

    /// Return a copy with strikethrough enabled.
    [[nodiscard]] constexpr Style with_strikethrough(bool v = true) const noexcept {
        Style s = *this;
        s.strikethrough = v;
        return s;
    }

    /// Return a copy with inverse / reverse-video enabled.
    [[nodiscard]] constexpr Style with_inverse(bool v = true) const noexcept {
        Style s = *this;
        s.inverse = v;
        return s;
    }

    /// Return a copy with conceal (SGR 8) enabled — occupy width, paint
    /// nothing. See the `conceal` field.
    [[nodiscard]] constexpr Style with_conceal(bool v = true) const noexcept {
        Style s = *this;
        s.conceal = v;
        return s;
    }

    /// Return a copy with the hardware-caret anchor set — see the
    /// `caret_anchor` field. Layout-only; never emitted as SGR.
    [[nodiscard]] constexpr Style with_caret_anchor(bool v = true) const noexcept {
        Style s = *this;
        s.caret_anchor = v;
        return s;
    }

    /// Return a copy with the DECSCUSR caret shape set — see the
    /// `caret_shape` field. Layout-only; never emitted as SGR.
    [[nodiscard]] constexpr Style with_caret_shape(uint8_t v) const noexcept {
        Style s = *this;
        s.caret_shape = v;
        return s;
    }

    // -- SGR generation ------------------------------------------------------

    /// Build the full ANSI SGR escape sequence for this style.
    /// Returns an empty string when no properties are set (no-op style).
    [[nodiscard]] std::string to_sgr() const {
        std::string params;

        auto append = [&](const std::string& code) {
            if (!params.empty()) params += ';';
            params += code;
        };

        // Attribute codes (SGR parameter order follows convention)
        if (bold)          append("1");
        if (dim)           append("2");
        if (italic)        append("3");
        if (underline)     append("4");
        if (inverse)       append("7");
        // NB: conceal (SGR 8) is deliberately NOT emitted here. It is
        // resolved at PAINT time (emit_cell_run substitutes a space) so it
        // works on every terminal regardless of SGR-8 support and stays
        // background-independent. Emitting \e[8m too would be redundant and
        // could double-hide on terminals that honour it.
        if (strikethrough) append("9");

        if (fg.has_value()) append(fg->fg_sgr());
        if (bg.has_value()) append(bg->bg_sgr());

        if (params.empty()) return {};
        return "\x1b[" + params + "m";
    }

    /// The universal SGR reset sequence.
    [[nodiscard]] static std::string reset_sgr() {
        return "\x1b[0m";
    }

    // -- Composition ---------------------------------------------------------

    /// Merge another style on top of this one (overlay pattern).
    /// The other style's *set* properties override this style's properties.
    /// Boolean attributes in `other` override when they are true.
    [[nodiscard]] constexpr Style merge(const Style& other) const noexcept {
        Style out = *this;

        if (other.fg.has_value())  out.fg = other.fg;
        if (other.bg.has_value())  out.bg = other.bg;

        if (other.bold)          out.bold          = true;
        if (other.dim)           out.dim           = true;
        if (other.italic)        out.italic        = true;
        if (other.underline)     out.underline     = true;
        if (other.strikethrough) out.strikethrough = true;
        if (other.inverse)       out.inverse       = true;
        if (other.conceal)       out.conceal       = true;
        if (other.caret_anchor)  out.caret_anchor  = true;
        if (other.caret_shape)   out.caret_shape   = other.caret_shape;

        return out;
    }

    /// Returns true when no properties are set (default-constructed style).
    [[nodiscard]] constexpr bool empty() const noexcept {
        return !fg.has_value() && !bg.has_value()
            && !bold && !dim && !italic && !underline
            && !strikethrough && !inverse && !conceal && !caret_anchor
            && caret_shape == 0;
    }

    constexpr auto operator<=>(const Style&) const = default;
};

// ============================================================================
// operator| - Merge two styles (left is base, right overlays)
// ============================================================================

[[nodiscard]] constexpr Style operator|(const Style& lhs, const Style& rhs) noexcept {
    return lhs.merge(rhs);
}

// ============================================================================
// Predefined constexpr styles
// ============================================================================

/// Bold text.
inline constexpr Style bold_style = Style{}.with_bold();

/// Dim / faint text.
inline constexpr Style dim_style = Style{}.with_dim();

/// Italic text.
inline constexpr Style italic_style = Style{}.with_italic();

/// Underlined text.
inline constexpr Style underline_style = Style{}.with_underline();

/// Strikethrough text.
inline constexpr Style strikethrough_style = Style{}.with_strikethrough();

/// Inverse / reverse-video text.
inline constexpr Style inverse_style = Style{}.with_inverse();

// -- Foreground color shorthand styles ---------------------------------------

inline constexpr Style fg_black   = Style{}.with_fg(Color::black());
inline constexpr Style fg_red     = Style{}.with_fg(Color::red());
inline constexpr Style fg_green   = Style{}.with_fg(Color::green());
inline constexpr Style fg_yellow  = Style{}.with_fg(Color::yellow());
inline constexpr Style fg_blue    = Style{}.with_fg(Color::blue());
inline constexpr Style fg_magenta = Style{}.with_fg(Color::magenta());
inline constexpr Style fg_cyan    = Style{}.with_fg(Color::cyan());
inline constexpr Style fg_white   = Style{}.with_fg(Color::white());

// -- Background color shorthand styles ---------------------------------------

inline constexpr Style bg_black   = Style{}.with_bg(Color::black());
inline constexpr Style bg_red     = Style{}.with_bg(Color::red());
inline constexpr Style bg_green   = Style{}.with_bg(Color::green());
inline constexpr Style bg_yellow  = Style{}.with_bg(Color::yellow());
inline constexpr Style bg_blue    = Style{}.with_bg(Color::blue());
inline constexpr Style bg_magenta = Style{}.with_bg(Color::magenta());
inline constexpr Style bg_cyan    = Style{}.with_bg(Color::cyan());
inline constexpr Style bg_white   = Style{}.with_bg(Color::white());

// ============================================================================
// Compile-time validation
// ============================================================================

static_assert(Style{}.empty(), "Default style must be empty");
static_assert(!bold_style.empty(), "Bold style must not be empty");
static_assert(bold_style.bold, "bold_style must have bold set");
static_assert(!bold_style.italic, "bold_style must not have italic set");

// Builder chaining
static_assert(Style{}.with_bold().with_italic().bold);
static_assert(Style{}.with_bold().with_italic().italic);

// Merge via operator|
static_assert((bold_style | italic_style).bold);
static_assert((bold_style | italic_style).italic);
static_assert((bold_style | fg_red).fg.has_value());
static_assert((bold_style | fg_red).bold);

// Overlay semantics: right-hand fg wins
static_assert((fg_red | fg_blue).fg == Color::blue());

// Trivially copyable (value type guarantee)
static_assert(std::is_trivially_copyable_v<Style>);

// ============================================================================
// fg() / bg() — Free functions for quick Style construction
// ============================================================================
// Usage:  text("hello") | bold_style | fg(Color::red()) | bg(Color::hex(0x222222))

[[nodiscard]] constexpr Style fg(Color c) noexcept { return Style{}.with_fg(c); }
[[nodiscard]] constexpr Style bg(Color c) noexcept { return Style{}.with_bg(c); }

// Short aliases — less typing for the most common attributes.
inline constexpr Style bold      = bold_style;
inline constexpr Style dim       = dim_style;
inline constexpr Style italic    = italic_style;
inline constexpr Style underline = underline_style;

} // namespace maya
