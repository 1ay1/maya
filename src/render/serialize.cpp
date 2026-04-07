#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8

#include <algorithm>
#include <cstdint>

namespace maya {

int content_height(const Canvas& canvas) noexcept {
    // O(1) fast path: if the canvas tracked max_y_ during painting,
    // we can skip the full scan entirely.
    int max_row = canvas.max_content_row();
    if (max_row >= 0) return max_row + 1;

    // Fallback: scan from bottom (only needed if nothing was painted).
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

    char ascii_buf[256];
    int ascii_len = 0;

    for (int y = y_begin; y < y_end; ++y) {
        if (y > y_begin) {
            // Flush the ASCII batch buffer before the row separator so that
            // the previous row's content precedes the \r\n in the output.
            if (ascii_len > 0) {
                out.append(ascii_buf, static_cast<size_t>(ascii_len));
                ascii_len = 0;
            }
            out += "\r\n";
        }

        const int row_base = y * W;
        for (int x = 0; x < W; ++x) {
            const uint64_t packed = cells[row_base + x];
            const auto ch = static_cast<char32_t>(packed & 0xFFFFFFFF);
            const auto sid = static_cast<uint16_t>((packed >> 32) & 0xFFFF);
            const auto w = static_cast<uint8_t>(packed >> 56);

            if (w == 2) [[unlikely]] continue;

            if (sid != current_style) [[unlikely]] {
                if (ascii_len > 0) {
                    out.append(ascii_buf, static_cast<size_t>(ascii_len));
                    ascii_len = 0;
                }
                out.append(pool.sgr(sid));
                current_style = sid;
            }

            if (ch < 0x80) [[likely]] {
                ascii_buf[ascii_len++] = static_cast<char>(ch);
                if (ascii_len == 256) [[unlikely]] {
                    out.append(ascii_buf, 256);
                    ascii_len = 0;
                }
            } else {
                if (ascii_len > 0) {
                    out.append(ascii_buf, static_cast<size_t>(ascii_len));
                    ascii_len = 0;
                }
                detail::encode_utf8(ch, out);
            }
        }
    }
    if (ascii_len > 0) {
        out.append(ascii_buf, static_cast<size_t>(ascii_len));
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

    // Batch cursor movement over unchanged rows instead of emitting
    // \r\n for every single row.  cursor_y tracks the row the terminal
    // cursor is physically on.
    int cursor_y = y_begin;

    for (int y = y_begin; y < y_end; ++y) {
        // Check if row changed
        bool changed = true;
        if (y < old_count && y < new_count)
            changed = (old_hashes[y] != new_hashes[y]);

        if (!changed) continue; // skip unchanged rows entirely

        // Move cursor from cursor_y to row y
        int delta = y - cursor_y;
        if (delta > 0) {
            out += "\r\n";
            if (delta > 1)
                ansi::write_cursor_down(out, delta - 1);
        } else {
            // Same row or first row — just go to column 1
            out += "\r";
        }

        const int row_base = y * W;
        // Find last non-space cell to avoid writing trailing blanks
        int last_col = W - 1;
        while (last_col >= 0) {
            Cell c = Cell::unpack(cells[row_base + last_col]);
            if (c.character != U' ' && c.character != 0) break;
            --last_col;
        }

        // Batch consecutive ASCII cells with the same style into a single
        // append() call, avoiding per-character string size checks.
        char ascii_buf[256];
        int ascii_len = 0;

        for (int x = 0; x <= last_col; ++x) {
            const uint64_t packed = cells[row_base + x];
            // Extract fields directly from packed value (avoid full unpack).
            const auto ch = static_cast<char32_t>(packed & 0xFFFFFFFF);
            const auto sid = static_cast<uint16_t>((packed >> 32) & 0xFFFF);
            const auto w = static_cast<uint8_t>(packed >> 56);

            if (w == 2) [[unlikely]] continue; // wide-char placeholder

            if (sid != current_style) [[unlikely]] {
                // Flush ASCII buffer before style change.
                if (ascii_len > 0) {
                    out.append(ascii_buf, static_cast<size_t>(ascii_len));
                    ascii_len = 0;
                }
                out.append(pool.sgr(sid));
                current_style = sid;
            }

            if (ch < 0x80) [[likely]] {
                ascii_buf[ascii_len++] = static_cast<char>(ch);
                if (ascii_len == 256) [[unlikely]] {
                    out.append(ascii_buf, 256);
                    ascii_len = 0;
                }
            } else {
                // Flush ASCII buffer, then encode non-ASCII character.
                if (ascii_len > 0) {
                    out.append(ascii_buf, static_cast<size_t>(ascii_len));
                    ascii_len = 0;
                }
                detail::encode_utf8(ch, out);
            }
        }
        // Flush remaining ASCII.
        if (ascii_len > 0) {
            out.append(ascii_buf, static_cast<size_t>(ascii_len));
        }
        // Erase remainder of line (no flash — content already written)
        out += "\x1b[0K";
        cursor_y = y;
    }

    // Move cursor to the last row of the live region (y_end - 1)
    // so the caller's subsequent cursor math is correct.
    int target = y_end - 1;
    if (target > cursor_y) {
        int delta = target - cursor_y;
        out += "\r\n";
        if (delta > 1)
            ansi::write_cursor_down(out, delta - 1);
    }

    out += "\x1b[?7h"; // re-enable DECAWM
    out += ansi::reset;
}

} // namespace maya
