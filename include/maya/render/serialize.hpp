#pragma once
// maya::render::serialize - Full canvas serialization to ANSI
//
// Converts every cell in the canvas to an ANSI byte stream — no diffing,
// no skipping, no front/back comparison. This is the approach used by Ink
// (and Claude Code): render fresh, erase previous output, write new output.
//
// Output format (row-major, left-to-right, top-to-bottom):
//   [SGR transition] [glyph] ... \r\n
//   [SGR transition] [glyph] ... \r\n
//   ...
//   \x1b[0m   (final SGR reset)
//
// The caller wraps this in sync_start / sync_end and precedes it with
// erase_lines(prev_height) to atomically replace the previous render.

#include <cstdint>
#include <string>

#include "canvas.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

/// Serialize the entire canvas to `out` as styled ANSI text.
/// Rows are separated by \r\n. A final \x1b[0m resets all attributes.
/// No cursor-positioning sequences are emitted — output flows left-to-right,
/// top-to-bottom, matching the terminal's natural cursor movement.
inline void serialize(const Canvas& canvas, const StylePool& pool, std::string& out) {
    const int W = canvas.width();
    const int H = canvas.height();

    if (W <= 0 || H <= 0) return;

    const uint64_t* cells = canvas.cells();
    uint16_t current_style = UINT16_MAX; // sentinel: no SGR emitted yet

    for (int y = 0; y < H; ++y) {
        if (y > 0) out += "\r\n";

        const int row_base = y * W;
        for (int x = 0; x < W; ++x) {
            const Cell cell = Cell::unpack(cells[row_base + x]);

            // Skip the second half of a wide character — already covered.
            if (cell.width == 2) [[unlikely]] continue;

            // Emit style transition if needed.
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

            // Encode the glyph.
            const char32_t cp = cell.character;
            if (cp < 0x80) [[likely]] {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }

    out += ansi::reset; // reset SGR at end of frame
}

} // namespace maya
