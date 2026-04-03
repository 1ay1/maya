#pragma once
// maya::render::diff - Frame differ: direct ANSI output, zero intermediate allocations
//
// Compares two canvases cell-by-cell within the damage region and writes
// a compact ANSI byte stream that transforms the front buffer's terminal
// state into the back buffer's visual state.
//
// Key design: instead of building a vector<RenderOp> and then serializing,
// we write ANSI sequences directly into the caller's string buffer. This
// eliminates every heap allocation that the old RenderOp path incurred —
// one per changed cell for cursor positioning, one per style change, one
// per character batch. On a typical 80x24 frame with 10% changed cells,
// that's ~192 fewer allocations per render cycle.
//
// The 64-bit packed cell comparison lets us skip unchanged cells with a
// single integer test — the innermost loop body is just a load + compare.

#include <cstdint>
#include <string>

#include "canvas.hpp"
#include "../core/simd.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

// ============================================================================
// UTF-8 encoding helper
// ============================================================================

namespace detail {

/// Encode a Unicode code point, appending its UTF-8 bytes to `out`.
[[gnu::always_inline]] inline void encode_utf8(char32_t cp, std::string& out) {
    if (cp < 0x80) [[likely]] {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += "\xEF\xBF\xBD";  // U+FFFD replacement character
    }
}

} // namespace detail

// ============================================================================
// diff - Compare two canvases; write ANSI update stream directly to `out`
// ============================================================================
// Algorithm:
//   1. Determine the effective damage region from the back canvas.
//   2. Iterate row-by-row, column-by-column.
//   3. Fast-path skip: if packed 64-bit values match, continue (no branch taken).
//   4. Emit cursor move only when not at the expected position (zero-alloc).
//   5. Emit style transition only when style changes (zero-alloc).
//   6. Write character bytes directly into `out`.
//
// All ANSI sequences are written with write_move_to() / apply_to() /
// transition_to() — none of them allocate heap memory.

inline void diff(
    const Canvas& old_canvas,
    const Canvas& new_canvas,
    const StylePool& pool,
    std::string& out)
{
    const int width  = new_canvas.width();
    const int height = new_canvas.height();

    if (width == 0 || height == 0) [[unlikely]] return;

    const Rect damage = new_canvas.damage();
    if (damage.size.is_zero()) [[unlikely]] return;

    // Clamp damage to canvas bounds.
    const int x0 = std::max(0, damage.left().value);
    const int y0 = std::max(0, damage.top().value);
    const int x1 = std::min(width,  damage.right().value);
    const int y1 = std::min(height, damage.bottom().value);

    if (x0 >= x1 || y0 >= y1) [[unlikely]] return;

    // Cursor and style state (unknown / sentinel at start).
    int      cursor_x        = -1;
    int      cursor_y        = -1;
    uint16_t current_style   = UINT16_MAX;  // sentinel: no style written yet

    const bool size_changed = (old_canvas.width() != width || old_canvas.height() != height);
    const uint64_t blank    = Cell{}.pack();

    // Pre-fetch the raw cell buffers for direct pointer arithmetic in the loop.
    const uint64_t* new_cells = new_canvas.cells();
    const uint64_t* old_cells = size_changed ? nullptr : old_canvas.cells();
    const int       old_w     = size_changed ? 0 : old_canvas.width();
    const int       old_h     = size_changed ? 0 : old_canvas.height();

    for (int y = y0; y < y1; ++y) {
        const int  new_row_base  = y * width;
        const int  old_row_base  = y * old_w;
        const bool old_row_valid = !size_changed && y < old_h;

        // ── Level-1 SIMD skip: entire unchanged row ──────────────────────
        // bulk_eq compares all cells in one AVX2/SSE2/NEON pass (4 cells/cycle).
        // Most rows in a quiescent TUI are completely unchanged — this skips
        // the inner loop entirely for them.
        if (old_row_valid && old_w == width && x0 == 0 && x1 == width) [[likely]] {
            if (simd::bulk_eq(new_cells + new_row_base, old_cells + old_row_base,
                              static_cast<std::size_t>(width)))
                continue;
        }

        // ── Level-2 SIMD skip: within-row unchanged runs ─────────────────
        // find_first_diff() jumps ahead to the next changed cell without
        // touching the intermediate cells at all. For rows with sparse
        // changes (e.g. a blinking cursor), this skips the bulk unchanged
        // prefix and suffix in O(N/4) instead of O(N).
        auto x  = static_cast<std::size_t>(x0);
        const auto xe = static_cast<std::size_t>(x1);

        // How many cells from old canvas are available on this row?
        const auto old_xe = old_row_valid
                          ? std::min(xe, static_cast<std::size_t>(old_w))
                          : static_cast<std::size_t>(0);

        while (x < xe) {
            // Skip unchanged prefix via SIMD (only when old data is available).
            if (old_row_valid && x < old_xe) {
                const std::size_t avail = old_xe - x;
                const std::size_t skip  = simd::find_first_diff(
                    new_cells + new_row_base + x,
                    old_cells + old_row_base + x,
                    avail);
                x += skip;
                // If we've exhausted the old-canvas coverage for this row,
                // continue into the scalar section for the remaining new cells.
                if (x >= xe) break;
                // If SIMD only advanced into cells beyond old coverage, fall
                // through to the per-cell check below.
            }

            const uint64_t new_packed = new_cells[new_row_base + x];
            const uint64_t old_packed = (old_row_valid && x < old_xe)
                                      ? old_cells[old_row_base + x]
                                      : blank;

            if (new_packed == old_packed) [[likely]] { ++x; continue; }

            const Cell cell = Cell::unpack(new_packed);

            // Wide-char second-half: the first-half write already covers it.
            if (cell.width == 2) [[unlikely]] { ++x; continue; }

            // Cursor positioning (zero-alloc).
            const int cx = static_cast<int>(x);
            if (cursor_x != cx || cursor_y != y) {
                ansi::write_move_to(out, cx + 1, y + 1);
                cursor_x = cx;
                cursor_y = y;
            }

            // Style transition (zero-alloc).
            if (cell.style_id != current_style) {
                const Style& new_style = pool.get(cell.style_id);
                if (current_style == UINT16_MAX) [[unlikely]] {
                    out += ansi::reset;
                    ansi::StyleApplier::apply_to(new_style, out);
                } else {
                    ansi::StyleApplier::transition_to(
                        pool.get(current_style), new_style, out);
                }
                current_style = cell.style_id;
            }

            detail::encode_utf8(cell.character, out);
            cursor_x += (cell.width == 1) ? 2 : 1;
            ++x;
        }
    }
}

} // namespace maya
