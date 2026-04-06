#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8

#include <algorithm>
#include <cstdint>

namespace maya {

int content_height(const Canvas& canvas) noexcept {
    const int W = canvas.width();
    const int H = canvas.height();
    const uint64_t* cells = canvas.cells();

    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            auto cell = Cell::unpack(cells[y * W + x]);
            if (cell.character != U' ' && cell.character != 0) return y + 1;
        }
    }
    return 1; // at least 1 row
}

void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows, int start_row) {
    const int W = canvas.width();
    const int total_rows = (rows > 0) ? std::min(rows, canvas.height()) : canvas.height();
    const int y_begin = std::clamp(start_row, 0, total_rows);
    const int y_end = total_rows;

    if (W <= 0 || y_begin >= y_end) return;

    const uint64_t* cells = canvas.cells();
    uint16_t current_style = UINT16_MAX; // sentinel: no SGR emitted yet

    // Disable auto-wrap (DECAWM reset) so that characters extending past
    // the right margin don't wrap to the next line.
    //
    // IMPORTANT: every code path after this point must re-enable DECAWM
    // before returning (see the "\x1b[?7h" at the end of this function).
    out += "\x1b[?7l";

    for (int y = y_begin; y < y_end; ++y) {
        if (y > y_begin) out += "\r\n";

        const int row_base = y * W;
        for (int x = 0; x < W; ++x) {
            const Cell cell = Cell::unpack(cells[row_base + x]);

            // Skip the second half of a wide character — already covered.
            if (cell.width == 2) [[unlikely]] continue;

            // Style transition — pre-cached SGR, single memcpy.
            if (cell.style_id != current_style) {
                out.append(pool.sgr(cell.style_id));
                current_style = cell.style_id;
            }

            // Encode the glyph — batch UTF-8 write.
            detail::encode_utf8(cell.character, out);
        }
    }

    // Re-enable auto-wrap.
    out += "\x1b[?7h";

    out += ansi::reset; // reset SGR at end of frame
}

void serialize_changed(const Canvas& canvas, const StylePool& pool,
                       std::string& out, int rows, int start_row,
                       const uint64_t* old_hashes, int old_count,
                       const uint64_t* new_hashes, int new_count) {
    const int W = canvas.width();
    const int total_rows = (rows > 0) ? std::min(rows, canvas.height()) : canvas.height();
    const int y_begin = std::clamp(start_row, 0, total_rows);
    const int y_end = total_rows;

    if (W <= 0 || y_begin >= y_end) return;

    const uint64_t* cells = canvas.cells();
    uint16_t current_style = UINT16_MAX;

    out += "\x1b[?7l"; // disable DECAWM

    bool first = true;
    for (int y = y_begin; y < y_end; ++y) {
        // Move cursor to this row
        if (!first) out += "\r\n";
        first = false;

        // Check if row changed
        bool changed = true;
        if (y < old_count && y < new_count)
            changed = (old_hashes[y] != new_hashes[y]);

        if (!changed) continue; // skip — cursor moves via \r\n on next row

        // Erase line and render row content
        out += "\x1b[2K\r";

        const int row_base = y * W;
        for (int x = 0; x < W; ++x) {
            const Cell cell = Cell::unpack(cells[row_base + x]);
            if (cell.width == 2) [[unlikely]] continue;
            if (cell.style_id != current_style) {
                out.append(pool.sgr(cell.style_id));
                current_style = cell.style_id;
            }
            detail::encode_utf8(cell.character, out);
        }
    }

    out += "\x1b[?7h"; // re-enable DECAWM
    out += ansi::reset;
}

} // namespace maya
