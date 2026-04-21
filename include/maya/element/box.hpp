#pragma once
// maya::element::box - Box element: a flex container with optional border
//
// BoxElement is the primary layout primitive. It arranges its children using
// a flexbox-inspired model (direction, wrapping, gap, alignment) and can
// optionally draw a border and background. It holds a vector<Element>,
// making the UI tree recursive.

#include "../core/types.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

#include <cstdint>
#include <vector>

namespace maya {

// Forward declaration -- full definition lives in element.hpp
// which includes this header after Element is defined.
struct Element;

// ============================================================================
// Flex layout enums
// ============================================================================

/// Primary axis direction (analogous to CSS flex-direction).
enum class FlexDirection : uint8_t {
    Row,            ///< Left to right (default).
    Column,         ///< Top to bottom.
    RowReverse,     ///< Right to left.
    ColumnReverse,  ///< Bottom to top.
};

/// Whether children wrap onto new lines when they overflow.
enum class FlexWrap : uint8_t {
    NoWrap,    ///< Single line; children may overflow (default).
    Wrap,      ///< Wrap onto next line.
    WrapReverse,
};

/// Cross-axis alignment (analogous to CSS align-items / align-self).
enum class Align : uint8_t {
    Start,
    Center,
    End,
    Stretch,   ///< Fill the cross axis (default).
    Baseline,
};

/// Main-axis distribution (analogous to CSS justify-content).
enum class Justify : uint8_t {
    Start,         ///< Pack children toward the start (default).
    Center,        ///< Center children along the main axis.
    End,           ///< Pack children toward the end.
    SpaceBetween,  ///< Equal space between children.
    SpaceAround,   ///< Equal space around children.
    SpaceEvenly,   ///< Equal space between and around children.
};

/// Overflow handling.
enum class Overflow : uint8_t {
    Visible,  ///< Children may render outside the box (default).
    Hidden,   ///< Clip children at the box boundary.
    Scroll,   ///< Clip with scroll indicators (future).
};

// Convenience constants for builder DSL readability.
inline constexpr auto Row            = FlexDirection::Row;
inline constexpr auto Column         = FlexDirection::Column;
inline constexpr auto RowReverse     = FlexDirection::RowReverse;
inline constexpr auto ColumnReverse  = FlexDirection::ColumnReverse;

// ============================================================================
// FlexStyle - All flexbox layout properties for a container or item
// ============================================================================
// Separated from visual style so that layout can be reasoned about
// independently. Every BoxElement carries one of these.

struct FlexStyle {
    FlexDirection direction = FlexDirection::Row;
    FlexWrap      wrap      = FlexWrap::NoWrap;
    Align         align_items   = Align::Stretch;
    Align         align_self    = Align::Start;
    Justify       justify       = Justify::Start;

    /// Flex grow factor (0 = don't grow).
    float grow   = 0.0f;
    /// Flex shrink factor (1 = shrink proportionally).
    float shrink = 1.0f;
    /// Flex basis (initial main-axis size before grow/shrink).
    Dimension basis{};

    /// Explicit width / height constraints.
    Dimension width{};
    Dimension height{};

    /// Minimum and maximum size constraints.
    Dimension min_width{};
    Dimension min_height{};
    Dimension max_width{};
    Dimension max_height{};

    /// Gap between children on the main axis (in columns/rows).
    int gap = 0;

    /// Padding and margin (in cells).
    Edges<int> padding{};
    Edges<int> margin{};
};

// ============================================================================
// BoxElement - A flex container node in the element tree
// ============================================================================
// Owns a FlexStyle for layout, a Style for visual appearance (bg color,
// text attributes applied to border text, etc.), a BorderConfig, and
// a vector of child Elements.
//
// BoxElement is forward-declared in element.hpp and defined here. The
// include order ensures that Element (the variant) is complete before
// this header is processed.

struct BoxElement {
    FlexStyle              layout{};
    Style                  style{};
    BorderConfig           border{.style = BorderStyle::None};
    Overflow               overflow = Overflow::Visible;
    bool                   is_stack = false;
    // No `{}` initializer here. With one, the compiler must instantiate
    // vector<Element>::vector() (and its `_GLIBCXX20_CONSTEXPR` destructor,
    // which does pointer arithmetic on Element*) while parsing this header —
    // but Element is still only forward-declared at this point. clang +
    // libstdc++ rejects that in C++20+; gcc tolerates it. Without the
    // initializer, vector<Element>'s special members are only instantiated
    // when BoxElement is itself default-constructed, by which time
    // element.hpp has finished defining Element.
    std::vector<Element>   children;

    // -- Convenience accessors -----------------------------------------------

    /// True if this box has a visible border.
    [[nodiscard]] bool has_border() const noexcept {
        return !border.empty();
    }

    /// Total horizontal space consumed by border + padding.
    [[nodiscard]] int inner_horizontal() const noexcept {
        int b = has_border() ? 2 : 0; // 1 cell each side if border is present
        return layout.padding.horizontal() + b;
    }

    /// Total vertical space consumed by border + padding.
    [[nodiscard]] int inner_vertical() const noexcept {
        int b = has_border() ? 2 : 0;
        return layout.padding.vertical() + b;
    }
};

} // namespace maya
