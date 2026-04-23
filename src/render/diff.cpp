#include "maya/render/diff.hpp"

#include <algorithm>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#  define MAYA_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 1)
#  define MAYA_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 1)
#  define MAYA_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define MAYA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
// MSVC lacks __builtin_prefetch; emit a native SSE prefetch. The GCC hint
// level we want (1 = "some temporal locality") maps best to _MM_HINT_T2
// (L3-only), matching the one-touch-per-frame access pattern of Cell rows
// in the diff loop. _mm_prefetch is read-only — MSVC has no write-prefetch
// intrinsic; PREFETCHW is available on AMD but we stay portable here.
// __assume used for branch hints: lets the optimizer know the expected
// edge without relying on PGO feedback.
#  include <intrin.h>
#  define MAYA_PREFETCH_R(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T2)
#  define MAYA_PREFETCH_W(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T2)
#  define MAYA_LIKELY(x)   (x)
#  define MAYA_UNLIKELY(x) (x)
#else
#  define MAYA_PREFETCH_R(addr) ((void)0)
#  define MAYA_PREFETCH_W(addr) ((void)0)
#  define MAYA_LIKELY(x)   (x)
#  define MAYA_UNLIKELY(x) (x)
#endif

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

    // Pre-reserve output buffer: worst case ~20 bytes/cell for style+char+cursor.
    // In practice most cells are unchanged; reserve for ~5% damage.
    const std::size_t estimated = static_cast<std::size_t>((x1 - x0) * (y1 - y0)) / 20 + 256;
    if (out.capacity() < out.size() + estimated)
        out.reserve(out.size() + estimated);

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

    // ASCII batch buffer — accumulates consecutive ASCII characters with the
    // same style to reduce per-character append() overhead in the inner loop.
    char ascii_buf[256];
    int  ascii_len = 0;

    // Lambda: flush the ASCII batch buffer.
    auto flush_ascii = [&] {
        if (ascii_len > 0) {
            out.append(ascii_buf, static_cast<std::size_t>(ascii_len));
            ascii_len = 0;
        }
    };

    for (int y = y0; y < y1; ++y) {
        const int  new_row_base  = y * width;
        const int  old_row_base  = y * old_w;
        const bool old_row_valid = !size_changed && y < old_h;

        // Prefetch the next row's data into L2 while processing this row.
        if (y + 1 < y1) [[likely]] {
            MAYA_PREFETCH_R(new_cells + (y + 1) * width);
            if (old_row_valid) MAYA_PREFETCH_R(old_cells + (y + 1) * old_w);
        }

        // ── Level-1 SIMD skip: entire unchanged row ──────────────────────
        if (old_row_valid && old_w == width && x0 == 0 && x1 == width) [[likely]] {
            if (simd::bulk_eq(new_cells + new_row_base, old_cells + old_row_base,
                              static_cast<std::size_t>(width)))
                continue;
        }

        // ── Pre-scan: find last non-blank column in new canvas ───────────
        // This enables the EL optimisation below: instead of writing each
        // trailing blank cell individually, one \x1b[K clears them all.
        int last_content = x0 - 1;
        for (int scan = x1 - 1; scan >= x0; --scan) {
            if (new_cells[new_row_base + scan] != blank) {
                last_content = scan;
                break;
            }
        }
        // Inner loop only needs to process up to last_content (inclusive).
        const auto content_end = static_cast<std::size_t>(last_content + 1);

        // ── Level-2 SIMD skip: within-row unchanged runs ─────────────────
        auto x  = static_cast<std::size_t>(x0);

        const auto old_xe = old_row_valid
                          ? std::min(content_end, static_cast<std::size_t>(old_w))
                          : static_cast<std::size_t>(0);

        while (x < content_end) {
            // Skip unchanged prefix via SIMD (only when old data is available).
            if (old_row_valid && x < old_xe) {
                const std::size_t avail = old_xe - x;
                const std::size_t skip  = simd::find_first_diff(
                    new_cells + new_row_base + x,
                    old_cells + old_row_base + x,
                    avail);
                if (skip > 0) {
                    // Flush ASCII buffer before skipping — cursor position
                    // will be discontinuous after the unchanged run.
                    flush_ascii();
                }
                x += skip;
                if (x >= content_end) break;
            }

            const uint64_t new_packed = new_cells[new_row_base + x];
            const uint64_t old_packed = (old_row_valid && x < old_xe)
                                      ? old_cells[old_row_base + x]
                                      : blank;

            if (new_packed == old_packed) [[likely]] {
                // Flush before gap in changed cells.
                flush_ascii();
                ++x; continue;
            }

            // Wide-char second-half: check packed width byte directly
            // (bits 56-63) without full unpack. Value 2 = placeholder.
            if ((new_packed >> 56) == 2) [[unlikely]] { ++x; continue; }

            // Cursor positioning — only emit CUP if cursor isn't already here.
            // After writing a character, cursor_x advances naturally, so
            // consecutive changed cells on the same row need no CUP.
            const int cx = static_cast<int>(x);
            if (cursor_x != cx || cursor_y != y) {
                flush_ascii();
                detail::write_cup(out, cx + 1, y + 1);
                cursor_x = cx;
                cursor_y = y;
            }

            // Extract style_id and character from packed value directly.
            const auto style_id = static_cast<uint16_t>((new_packed >> 32) & 0xFFFF);
            const auto character = static_cast<char32_t>(new_packed & 0xFFFFFFFF);
            const auto cell_w = static_cast<uint8_t>(new_packed >> 56);

            // Style transition — pre-cached SGR, single memcpy.
            if (style_id != current_style) {
                flush_ascii();
                out.append(pool.sgr(style_id));
                current_style = style_id;
            }

            // Character — batch ASCII into buffer, encode non-ASCII directly.
            if (character < 0x80) [[likely]] {
                ascii_buf[ascii_len++] = static_cast<char>(character);
                if (ascii_len == 256) [[unlikely]] {
                    out.append(ascii_buf, 256);
                    ascii_len = 0;
                }
            } else {
                flush_ascii();
                detail::encode_utf8(character, out);
            }
            // Wide-char first half (width==1) occupies 2 columns; normal chars 1.
            cursor_x += (cell_w == 1) ? 2 : 1;
            ++x;
        }

        // ── EL optimisation: clear trailing old content in one shot ──────
        // If old canvas had non-blank cells beyond last_content, a single
        // \x1b[K (erase to end of line) replaces what would otherwise be
        // dozens of individual space writes + cursor moves.
        if (old_row_valid && last_content < static_cast<int>(x1) - 1) {
            const auto trail_start = static_cast<std::size_t>(last_content + 1);
            const auto trail_end   = std::min(static_cast<std::size_t>(x1),
                                              static_cast<std::size_t>(old_w));
            // Use SIMD to check if old trailing region is all blank.
            // bulk_eq against a blank row would require a blank buffer;
            // instead use find_first_diff against the new (already blank)
            // region — if old differs from new anywhere, we need EL.
            bool need_el = false;
            if (trail_start < trail_end) {
                const std::size_t trail_len = trail_end - trail_start;
                const std::size_t same = simd::find_first_diff(
                    new_cells + new_row_base + trail_start,
                    old_cells + old_row_base + trail_start,
                    trail_len);
                need_el = (same < trail_len);
            }
            if (need_el) {
                flush_ascii();
                const int el_col = last_content + 1;
                if (cursor_x != el_col || cursor_y != y) {
                    detail::write_cup(out, el_col + 1, y + 1);
                }
                // EL uses the current background — reset to default first.
                if (current_style != 0) {
                    out.append(pool.sgr(0));
                    current_style = 0;
                }
                out += "\x1b[K";
                cursor_x = el_col;
                cursor_y = y;
            }
        }
    }
    // Flush remaining ASCII.
    flush_ascii();
}

} // namespace maya
