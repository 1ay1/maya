#include "maya/render/diff.hpp"

#include <algorithm>
#include <cstdint>

namespace maya {

void diff(
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

    // Cursor and style state (unknown at start).
    int      cursor_x      = -1;
    int      cursor_y      = -1;
    uint16_t current_style = UINT16_MAX; // sentinel: no style written yet

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
        if (old_row_valid && old_w == width && x0 == 0 && x1 == width) [[likely]] {
            if (simd::bulk_eq(new_cells + new_row_base, old_cells + old_row_base,
                              static_cast<std::size_t>(width)))
                continue;
        }

        // ── Level-2 SIMD skip: within-row unchanged runs ─────────────────
        auto x  = static_cast<std::size_t>(x0);
        const auto xe = static_cast<std::size_t>(x1);

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
                if (x >= xe) break;
            }

            const uint64_t new_packed = new_cells[new_row_base + x];
            const uint64_t old_packed = (old_row_valid && x < old_xe)
                                      ? old_cells[old_row_base + x]
                                      : blank;

            if (new_packed == old_packed) [[likely]] { ++x; continue; }

            const Cell cell = Cell::unpack(new_packed);

            // Wide-char second-half: the first-half write already covers it.
            if (cell.width == 2) [[unlikely]] { ++x; continue; }

            // Cursor positioning — single append via stack buffer.
            const int cx = static_cast<int>(x);
            if (cursor_x != cx || cursor_y != y) {
                detail::write_cup(out, cx + 1, y + 1);
                cursor_x = cx;
                cursor_y = y;
            }

            // Style transition — pre-cached SGR, single memcpy.
            if (cell.style_id != current_style) {
                out.append(pool.sgr(cell.style_id));
                current_style = cell.style_id;
            }

            // Character — batch UTF-8 encoding.
            detail::encode_utf8(cell.character, out);
            cursor_x += (cell.width == 1) ? 2 : 1;
            ++x;
        }
    }
}

} // namespace maya
