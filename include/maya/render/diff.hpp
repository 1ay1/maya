#pragma once
// maya::render::diff - Frame differ for minimal terminal updates
//
// Compares two canvases cell-by-cell within the damage region and produces
// a compact sequence of RenderOps that transform the front buffer's
// terminal state into the back buffer's visual state.
//
// The diff exploits 64-bit packed cell comparison to skip unchanged cells
// in a single integer test. It tracks the "current" cursor position and
// active style to suppress redundant cursor-move and style-change ops.
// Consecutive identical-style character writes are batched into single
// Write operations to minimize escape sequence overhead.

#include <cstdint>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "../terminal/ansi.hpp"
#include "../terminal/writer.hpp"

namespace maya {

// ============================================================================
// DiffResult - The output of a frame diff
// ============================================================================
// A flat vector of RenderOps ready to be pushed into a Writer for flushing.

struct DiffResult {
    std::vector<RenderOp> ops;

    [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ops.size(); }
};

// ============================================================================
// UTF-8 encoding helper
// ============================================================================

namespace detail {

/// Encode a single Unicode code point to UTF-8, appending to `out`.
inline void encode_utf8(char32_t cp, std::string& out) {
    if (cp < 0x80) {
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
        // Invalid code point -- emit replacement character U+FFFD.
        out += "\xEF\xBF\xBD";
    }
}

} // namespace detail

// ============================================================================
// diff - Compare two canvases and produce RenderOps
// ============================================================================
// The algorithm:
//   1. Determine the effective damage region (intersection of back canvas
//      damage with canvas bounds).
//   2. Iterate row-by-row, column-by-column through the damage region.
//   3. Skip cells whose packed 64-bit values match between old and new.
//   4. For changed cells, emit cursor positioning (only when the cursor is
//      not already at the right position), style transitions (only when the
//      style differs from the last emitted style), and character data.
//   5. Batch consecutive character writes on the same row into a single
//      Write op to reduce per-character overhead.

[[nodiscard]] inline DiffResult diff(
    const Canvas& old_canvas,
    const Canvas& new_canvas,
    const StylePool& pool)
{
    DiffResult result;

    int width  = new_canvas.width();
    int height = new_canvas.height();

    if (width == 0 || height == 0) return result;

    // Determine the region we need to scan.
    Rect damage = new_canvas.damage();
    if (damage.size.is_zero()) return result;

    // Clamp damage to canvas bounds.
    int x0 = std::max(0, damage.left().value);
    int y0 = std::max(0, damage.top().value);
    int x1 = std::min(width, damage.right().value);
    int y1 = std::min(height, damage.bottom().value);

    if (x0 >= x1 || y0 >= y1) return result;

    // State tracking to minimize redundant ops.
    int cursor_x = -1;  // unknown position
    int cursor_y = -1;
    uint16_t current_style_id = UINT16_MAX;  // sentinel: no style applied yet

    // Pre-allocate a reasonable estimate.
    result.ops.reserve(static_cast<std::size_t>((y1 - y0) * 4));

    // Pending write accumulator: we batch consecutive characters on the
    // same row with the same style into a single Write op.
    std::string pending_text;
    int pending_start_x = -1;
    int pending_y = -1;

    auto flush_pending = [&]() {
        if (pending_text.empty()) return;
        result.ops.emplace_back(render_op::Write{std::move(pending_text)});
        pending_text.clear();
        pending_start_x = -1;
        pending_y = -1;
    };

    bool size_changed = (old_canvas.width() != width || old_canvas.height() != height);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint64_t new_packed = new_canvas.get_packed(x, y);
            uint64_t old_packed = (!size_changed && x < old_canvas.width() && y < old_canvas.height())
                                ? old_canvas.get_packed(x, y)
                                : Cell{}.pack();

            // Fast path: cell unchanged -- skip it.
            if (new_packed == old_packed) {
                flush_pending();
                continue;
            }

            Cell cell = Cell::unpack(new_packed);

            // Skip wide-char second-half placeholders; the first-half
            // write already covers both columns.
            if (cell.width == 2) continue;

            // Emit cursor positioning if we're not at the expected position.
            if (cursor_x != x || cursor_y != y) {
                flush_pending();
                // Use absolute CUP (1-based row, col).
                result.ops.emplace_back(render_op::Write{
                    ansi::move_to(x + 1, y + 1)
                });
                cursor_x = x;
                cursor_y = y;
            }

            // Emit style transition if needed.
            if (cell.style_id != current_style_id) {
                flush_pending();

                const Style& new_style = pool.get(cell.style_id);
                std::string sgr;

                if (current_style_id == UINT16_MAX) {
                    // First style application -- reset then apply from scratch.
                    std::string full;
                    full += ansi::reset;
                    full += ansi::StyleApplier::apply(new_style);
                    sgr = std::move(full);
                } else {
                    // Incremental transition from current style.
                    const Style& prev_style = pool.get(current_style_id);
                    sgr = ansi::StyleApplier::transition(prev_style, new_style);
                }

                if (!sgr.empty()) {
                    result.ops.emplace_back(render_op::StyleStr{std::move(sgr)});
                }
                current_style_id = cell.style_id;
            }

            // Append this character to the pending write batch.
            detail::encode_utf8(cell.character, pending_text);
            if (pending_start_x < 0) {
                pending_start_x = x;
                pending_y = y;
            }

            // Advance cursor tracking.
            int advance = (cell.width == 1) ? 2 : 1;
            cursor_x += advance;
        }

        // Flush at end of each row.
        flush_pending();
    }

    flush_pending();

    return result;
}

// ============================================================================
// optimize - Peephole optimization pass over a DiffResult
// ============================================================================
// Merges consecutive compatible operations to further reduce the byte
// count sent to the terminal.

[[nodiscard]] inline DiffResult optimize(DiffResult input) {
    if (input.ops.size() < 2) return input;

    DiffResult output;
    output.ops.reserve(input.ops.size());

    for (auto& op : input.ops) {
        if (!output.ops.empty()) {
            auto& last = output.ops.back();

            // Merge consecutive Write ops.
            if (auto* a = std::get_if<render_op::Write>(&last)) {
                if (auto* b = std::get_if<render_op::Write>(&op)) {
                    a->text += b->text;
                    continue;
                }
            }

            // Merge consecutive StyleStr ops.
            if (auto* a = std::get_if<render_op::StyleStr>(&last)) {
                if (auto* b = std::get_if<render_op::StyleStr>(&op)) {
                    a->sgr += b->sgr;
                    continue;
                }
            }

            // Merge consecutive CursorMove ops.
            if (auto* a = std::get_if<render_op::CursorMove>(&last)) {
                if (auto* b = std::get_if<render_op::CursorMove>(&op)) {
                    a->dx += b->dx;
                    a->dy += b->dy;
                    continue;
                }
            }

            // Cancel adjacent CursorHide + CursorShow (and vice versa).
            bool hide_then_show =
                std::holds_alternative<render_op::CursorHide>(last) &&
                std::holds_alternative<render_op::CursorShow>(op);
            bool show_then_hide =
                std::holds_alternative<render_op::CursorShow>(last) &&
                std::holds_alternative<render_op::CursorHide>(op);
            if (hide_then_show || show_then_hide) {
                output.ops.pop_back();
                continue;
            }
        }

        output.ops.push_back(std::move(op));
    }

    return output;
}

} // namespace maya
