#pragma once
// maya::layout::yoga - Simplified Yoga-like flexbox layout engine
//
// A pure C++26 implementation of the CSS Flexbox layout algorithm,
// tailored for terminal UIs. All arithmetic is performed in integer
// terminal cells. The engine operates on a flat vector of LayoutNodes
// identified by index, avoiding pointer-based trees entirely.
//
// Supports: row/column direction, flex-grow/shrink, wrapping, alignment,
// justify-content, gap, padding/margin/border, min/max constraints,
// nested layouts via recursion, percentage dimensions, and Display::None.
//
// Usage:
//   std::vector<LayoutNode> nodes;
//   // ... build your tree by pushing nodes and setting children indices ...
//   layout::compute(nodes, /*root=*/0, /*available_width=*/80);
//   // Each node's `computed` rect now contains its final position and size.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <maya/core/types.hpp>

namespace maya::layout {

// ============================================================================
// Enums
// ============================================================================

/// Main axis direction for flex layout.
enum class FlexDirection : uint8_t {
    Column,
    ColumnReverse,
    Row,
    RowReverse,
};

/// Whether flex items wrap onto multiple lines.
enum class FlexWrap : uint8_t {
    NoWrap,
    Wrap,
    WrapReverse,
};

/// Cross-axis alignment for children, or self-alignment override.
/// Auto is used only for align_self to mean "inherit from parent's align_items".
enum class Align : uint8_t {
    Auto,
    Start,
    End,
    Center,
    Stretch,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

/// Main-axis distribution of remaining space.
using Justify = Align;

/// Overflow behaviour (reserved for scroll support).
enum class Overflow : uint8_t {
    Visible,
    Hidden,
    Scroll,
};

/// Whether a node participates in layout.
enum class Display : uint8_t {
    Flex,
    None,
};

// ============================================================================
// FlexStyle - all flexbox properties for a single node
// ============================================================================

struct FlexStyle {
    FlexDirection direction     = FlexDirection::Column;
    FlexWrap      wrap          = FlexWrap::NoWrap;
    Align         align_items   = Align::Stretch;
    Align         align_self    = Align::Auto;
    Justify       justify_content = Justify::Start;

    float flex_grow   = 0.0f;
    float flex_shrink = 1.0f;

    Dimension flex_basis = Dimension::auto_();

    Dimension width      = Dimension::auto_();
    Dimension height     = Dimension::auto_();
    Dimension min_width  = Dimension::auto_();
    Dimension max_width  = Dimension::auto_();
    Dimension min_height = Dimension::auto_();
    Dimension max_height = Dimension::auto_();

    Edges<int> margin{};
    Edges<int> padding{};
    Edges<int> border{};

    int gap = 0;

    Overflow overflow = Overflow::Visible;
    Display  display  = Display::Flex;
};

// ============================================================================
// LayoutNode - a node in the layout tree
// ============================================================================

// Lightweight measure function — avoids std::function's type-erasure overhead.
// A function pointer + opaque context replaces the 32-byte std::function with
// a 16-byte pair that never heap-allocates.
struct MeasureFn {
    using Fn = Size(*)(const void* ctx, int max_width);
    Fn          fn  = nullptr;
    const void* ctx = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return fn != nullptr; }
    [[nodiscard]] Size operator()(int max_width) const { return fn(ctx, max_width); }
};

struct LayoutNode {
    FlexStyle                style;
    std::vector<std::size_t> children;
    MeasureFn                measure;
    Rect                     computed = {};
};

// ============================================================================
// Internal helpers
// ============================================================================

namespace detail {

/// True when the main axis runs horizontally.
[[gnu::always_inline]] [[nodiscard]] constexpr bool is_row(FlexDirection d) noexcept {
    return d == FlexDirection::Row || d == FlexDirection::RowReverse;
}

/// True when the main axis is reversed.
[[nodiscard]] constexpr bool is_reverse(FlexDirection d) noexcept {
    return d == FlexDirection::RowReverse || d == FlexDirection::ColumnReverse;
}

/// Clamp `v` between `lo` and `hi`, where either bound may be Dimension::Auto
/// (meaning unconstrained). `parent` is the reference for percentage resolution.
[[gnu::always_inline]] [[nodiscard]] constexpr int clamp_dim(int v, Dimension lo, Dimension hi, int parent) noexcept {
    int low  = lo.is_auto() ? 0          : lo.resolve(parent);
    int high = hi.is_auto() ? 0x7FFFFFFF : hi.resolve(parent);
    return std::clamp(v, low, high);
}

/// Total horizontal space consumed by margin + border + padding.
[[nodiscard]] constexpr int outer_horizontal(const FlexStyle& s) noexcept {
    return s.margin.horizontal() + s.border.horizontal() + s.padding.horizontal();
}

/// Total vertical space consumed by margin + border + padding.
[[nodiscard]] constexpr int outer_vertical(const FlexStyle& s) noexcept {
    return s.margin.vertical() + s.border.vertical() + s.padding.vertical();
}

/// Horizontal inset: border + padding (space inside the box available to children).
[[nodiscard]] constexpr int inner_horizontal(const FlexStyle& s) noexcept {
    return s.border.horizontal() + s.padding.horizontal();
}

/// Vertical inset: border + padding.
[[nodiscard]] constexpr int inner_vertical(const FlexStyle& s) noexcept {
    return s.border.vertical() + s.padding.vertical();
}

/// Left inset: border.left + padding.left.
[[nodiscard]] constexpr int inner_left(const FlexStyle& s) noexcept {
    return s.border.left + s.padding.left;
}

/// Top inset: border.top + padding.top.
[[nodiscard]] constexpr int inner_top(const FlexStyle& s) noexcept {
    return s.border.top + s.padding.top;
}

// --------------------------------------------------------------------------
// Per-flex-line item tracking during the algorithm
// --------------------------------------------------------------------------

struct FlexItem {
    std::size_t index;          // Index into the nodes vector
    int         hypothetical;   // Hypothetical main size before grow/shrink
    int         main;           // Final main size after grow/shrink
    int         cross;          // Final cross size
    int         main_offset;    // Offset along main axis (set during positioning)
    int         cross_offset;   // Offset along cross axis
};

struct FlexLine {
    std::vector<FlexItem> items;
    int main_size  = 0;  // Total main size of the line (sum of item mains + gaps)
    int cross_size = 0;  // Max cross size of items in the line
};

// --------------------------------------------------------------------------
// Core recursive layout
// --------------------------------------------------------------------------

void compute_node(
    std::vector<LayoutNode>& nodes,
    std::size_t idx,
    int available_width,
    int available_height,
    int parent_width,
    int parent_height);

/// Resolve a node's definite main/cross sizes given the parent context.
/// Returns (resolved_width, resolved_height), with -1 meaning "not yet determined".
[[nodiscard]] std::pair<int, int> resolve_definite_size(
    const FlexStyle& style,
    int parent_width,
    int parent_height) noexcept;

} // namespace detail

// ============================================================================
// Public API
// ============================================================================

/// Compute the layout for the tree rooted at `root` within `available_width`
/// terminal columns. After this call, every reachable node's `computed` field
/// contains its final position (relative to its parent) and size.
///
/// `available_height` defaults to a large value (unbounded vertical scroll).
/// Pass an explicit height to constrain vertical layout.
void compute(
    std::vector<LayoutNode>& nodes,
    std::size_t root,
    int available_width,
    int available_height = 0x7FFFFFFF);

/// Convert all child positions from parent-relative to absolute screen
/// coordinates. Call this after `compute()` if you need absolute positions
/// for rendering.
void resolve_absolute(
    std::vector<LayoutNode>& nodes,
    std::size_t idx,
    int offset_x = 0,
    int offset_y = 0);

} // namespace maya::layout
