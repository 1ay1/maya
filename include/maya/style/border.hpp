#pragma once
// maya::style::border - Border character sets and configuration
//
// Provides eight named border styles with Unicode box-drawing characters,
// a constexpr lookup function, and a BorderConfig struct for per-side
// enable/disable, per-side color, and optional border title text.

#include "color.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace maya {

// ============================================================================
// BorderStyle - Named visual styles for box borders
// ============================================================================

enum class BorderStyle : uint8_t {
    None,           ///< No border at all.
    Single,         ///< ┌─┐│┘─└│  Standard single-line box drawing.
    Double,         ///< ╔═╗║╝═╚║  Double-line box drawing.
    Round,          ///< ╭─╮│╯─╰│  Rounded corners, single lines.
    Bold,           ///< ┏━┓┃┛━┗┃  Heavy / bold single-line.
    SingleDouble,   ///< ╓─╖║╜─╙║  Single horizontal, double vertical.
    DoubleSingle,   ///< ╒═╕│╛═╘│  Double horizontal, single vertical.
    Classic,        ///< +-+|+-+|   ASCII-only fallback.
    Arrow,          ///< ↑→↓←       Arrow characters on each side.
};

// ============================================================================
// BorderChars - The eight characters that form a box border
// ============================================================================
// Layout reference:
//   top_left ── top ── top_right
//     │                   │
//    left              right
//     │                   │
//   bottom_left ─ bottom ─ bottom_right

struct BorderChars {
    std::string_view top_left;
    std::string_view top;
    std::string_view top_right;
    std::string_view right;
    std::string_view bottom_right;
    std::string_view bottom;
    std::string_view bottom_left;
    std::string_view left;
};

// ============================================================================
// get_border_chars - constexpr lookup of border characters by style
// ============================================================================

[[nodiscard]] constexpr BorderChars get_border_chars(BorderStyle style) noexcept {
    switch (style) {
        case BorderStyle::None:
            return {"", "", "", "", "", "", "", ""};

        case BorderStyle::Single:
            return {
                "\u250C", "\u2500", "\u2510",  // ┌ ─ ┐
                "\u2502",                       // │
                "\u2518", "\u2500", "\u2514",  // ┘ ─ └
                "\u2502"                        // │
            };

        case BorderStyle::Double:
            return {
                "\u2554", "\u2550", "\u2557",  // ╔ ═ ╗
                "\u2551",                       // ║
                "\u255D", "\u2550", "\u255A",  // ╝ ═ ╚
                "\u2551"                        // ║
            };

        case BorderStyle::Round:
            return {
                "\u256D", "\u2500", "\u256E",  // ╭ ─ ╮
                "\u2502",                       // │
                "\u256F", "\u2500", "\u2570",  // ╯ ─ ╰
                "\u2502"                        // │
            };

        case BorderStyle::Bold:
            return {
                "\u250F", "\u2501", "\u2513",  // ┏ ━ ┓
                "\u2503",                       // ┃
                "\u251B", "\u2501", "\u2517",  // ┛ ━ ┗
                "\u2503"                        // ┃
            };

        case BorderStyle::SingleDouble:
            return {
                "\u2553", "\u2500", "\u2556",  // ╓ ─ ╖
                "\u2551",                       // ║
                "\u255C", "\u2500", "\u2559",  // ╜ ─ ╙
                "\u2551"                        // ║
            };

        case BorderStyle::DoubleSingle:
            return {
                "\u2552", "\u2550", "\u2555",  // ╒ ═ ╕
                "\u2502",                       // │
                "\u255B", "\u2550", "\u2558",  // ╛ ═ ╘
                "\u2502"                        // │
            };

        case BorderStyle::Classic:
            return {"+", "-", "+", "|", "+", "-", "+", "|"};

        case BorderStyle::Arrow:
            return {
                "\u2191", "\u2191", "\u2191",  // ↑ ↑ ↑
                "\u2192",                       // →
                "\u2193", "\u2193", "\u2193",  // ↓ ↓ ↓
                "\u2190"                        // ←
            };
    }
    __builtin_unreachable();
}

// ============================================================================
// BorderSides - Per-side enable / disable flags
// ============================================================================

struct BorderSides {
    bool top    = true;
    bool right  = true;
    bool bottom = true;
    bool left   = true;

    /// All sides enabled (default).
    [[nodiscard]] static constexpr BorderSides all() noexcept {
        return {true, true, true, true};
    }

    /// No sides enabled.
    [[nodiscard]] static constexpr BorderSides none() noexcept {
        return {false, false, false, false};
    }

    /// Only horizontal (top + bottom) sides.
    [[nodiscard]] static constexpr BorderSides horizontal() noexcept {
        return {true, false, true, false};
    }

    /// Only vertical (left + right) sides.
    [[nodiscard]] static constexpr BorderSides vertical() noexcept {
        return {false, true, false, true};
    }

    constexpr auto operator<=>(const BorderSides&) const = default;
};

// ============================================================================
// Border text decoration (title / footer on the border edge)
// ============================================================================

/// Where the text appears on the border.
enum class BorderTextPos : uint8_t {
    Top,
    Bottom,
};

/// Horizontal alignment of border text within its edge.
enum class BorderTextAlign : uint8_t {
    Start,
    Center,
    End,
};

/// A snippet of text rendered inline on a border edge.
struct BorderText {
    std::string     content{};
    BorderTextPos   position = BorderTextPos::Top;
    BorderTextAlign align    = BorderTextAlign::Start;

    /// Signed offset from the aligned position (in columns).
    /// Positive moves toward End; negative moves toward Start.
    int offset = 0;
};

// ============================================================================
// BorderColors - Optional per-side color overrides
// ============================================================================

struct BorderColors {
    std::optional<Color> top{};
    std::optional<Color> right{};
    std::optional<Color> bottom{};
    std::optional<Color> left{};

    /// Set all four sides to the same color.
    [[nodiscard]] static constexpr BorderColors uniform(Color c) noexcept {
        return {c, c, c, c};
    }

    constexpr auto operator<=>(const BorderColors&) const = default;
};

// ============================================================================
// BorderConfig - Full border specification
// ============================================================================
// Combines the visual style, per-side toggles, per-side colors, and an
// optional border text (title or footer).
//
// Usage:
//   auto cfg = BorderConfig{
//       .style  = BorderStyle::Round,
//       .sides  = BorderSides::all(),
//       .colors = BorderColors::uniform(Color::cyan()),
//       .text   = BorderText{.content = " Title ", .align = BorderTextAlign::Center},
//   };

struct BorderConfig {
    BorderStyle             style  = BorderStyle::Single;
    BorderSides             sides  = BorderSides::all();
    BorderColors            colors{};
    std::optional<BorderText> text{};

    /// Convenience: look up the character set implied by this config.
    [[nodiscard]] constexpr BorderChars chars() const noexcept {
        return get_border_chars(style);
    }

    /// True when no border will be drawn (None style or all sides disabled).
    [[nodiscard]] constexpr bool empty() const noexcept {
        if (style == BorderStyle::None) return true;
        return !sides.top && !sides.right && !sides.bottom && !sides.left;
    }
};

// ============================================================================
// Compile-time validation
// ============================================================================

// BorderChars lookup
static_assert(get_border_chars(BorderStyle::Classic).top_left == "+");
static_assert(get_border_chars(BorderStyle::Classic).top      == "-");
static_assert(get_border_chars(BorderStyle::Classic).right    == "|");
static_assert(get_border_chars(BorderStyle::None).top_left    == "");

// BorderSides factories
static_assert(BorderSides::all()  == BorderSides{true, true, true, true});
static_assert(BorderSides::none() == BorderSides{false, false, false, false});
static_assert(BorderSides::horizontal().top  && !BorderSides::horizontal().right);
static_assert(BorderSides::vertical().right  && !BorderSides::vertical().top);

// BorderConfig emptiness
static_assert(BorderConfig{.style = BorderStyle::None}.empty());
static_assert(BorderConfig{.sides = BorderSides::none()}.empty());
static_assert(!BorderConfig{}.empty());

// BorderColors uniform factory
static_assert(BorderColors::uniform(Color::red()).top   == Color::red());
static_assert(BorderColors::uniform(Color::red()).left  == Color::red());

} // namespace maya
