#pragma once
// maya::render::renderer - Tree walker that renders an element tree to a canvas
//
// The renderer bridges the element/layout world and the canvas/cell world.
// It walks the element tree, runs layout to compute positions and sizes,
// then paints each element's visual representation onto the canvas:
//   - BoxElement: border lines, background fill, recursive children
//   - TextElement: styled text with word-wrap/truncation
//   - ElementList: transparent recursion into fragment children
//
// Clip regions are pushed for overflow:hidden/scroll containers so that
// children that extend beyond the box boundary are silently discarded.

#include <cstddef>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../layout/yoga.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

namespace maya {

namespace render_detail {

// ============================================================================
// Layout tree builder
// ============================================================================

std::size_t build_layout_tree(
    const Element& elem,
    std::vector<layout::LayoutNode>& nodes,
    const Theme& theme);

// ============================================================================
// Painting helpers
// ============================================================================

void paint_border(
    Canvas& canvas,
    const BorderConfig& border,
    const Rect& rect,
    uint16_t style_id);

// ============================================================================
// Recursive element painter
// ============================================================================

void paint_element(
    const Element& elem,
    Canvas& canvas,
    StylePool& pool,
    const std::vector<layout::LayoutNode>& layout_nodes,
    std::size_t layout_idx,
    int offset_x,
    int offset_y);

} // namespace render_detail

// ============================================================================
// render_tree - Public API: render an element tree onto a canvas
// ============================================================================

/// Render an element tree onto a canvas.
/// When auto_height is true, the root node's height is left unconstrained
/// so layout sizes to content (inline / scrollback-preserving mode).
void render_tree(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    const Theme& theme,
    bool auto_height = false);

} // namespace maya
