#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8
// simd::bulk_eq comes from canvas.hpp → core/simd.hpp
#include "maya/terminal/ansi.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace maya {

void InlineFrameState::commit_prefix(int rows) noexcept {
    if (rows <= 0 || prev_rows <= 0 || prev_width <= 0) return;
    if (rows >= prev_rows) { reset(); return; }

    const std::size_t W = static_cast<std::size_t>(prev_width);
    const std::size_t shift = static_cast<std::size_t>(rows) * W;
    const std::size_t remaining = static_cast<std::size_t>(prev_rows - rows) * W;

    uint64_t* data = prev_cells.data();
    if (data != nullptr && shift + remaining <= prev_cells.size()) {
        std::memmove(data, data + shift, remaining * sizeof(uint64_t));
    }
    prev_rows -= rows;
}

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

        // Trim trailing blanks: find last non-space cell to avoid writing
        // trailing spaces. A styled space (style_id != 0) is kept — it may
        // carry inverse, background color, or other visual attributes.
        int last_col = W - 1;
        while (last_col >= 0) {
            const uint64_t p = cells[row_base + last_col];
            const auto c = static_cast<char32_t>(p & 0xFFFFFFFF);
            const auto s = static_cast<uint16_t>((p >> 32) & 0xFFFF);
            if ((c != U' ' && c != 0) || s != 0) break;
            --last_col;
        }

        for (int x = 0; x <= last_col; ++x) {
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
        // Flush remaining ASCII for this row, then erase to end of line.
        // EL 0 cleans up any stale content from previous frames that was
        // wider than the current row.
        if (ascii_len > 0) {
            out.append(ascii_buf, static_cast<size_t>(ascii_len));
            ascii_len = 0;
        }
        out += "\x1b[K";
    }
    // Re-enable auto-wrap.
    out += "\x1b[?7h";

    out += ansi::reset; // reset SGR at end of frame
}

// ============================================================================
// compose_inline_frame
// ============================================================================

void compose_inline_frame(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState& state,
                          std::string& out)
{
    const int W = canvas.width();
    if (W <= 0 || content_rows <= 0 || term_h <= 0) return;

    // Width change invalidates the cached cell buffer — row layouts shift.
    if (state.prev_width != W) state.reset();

    const uint64_t* cells = canvas.cells();
    const int prev_rows       = state.prev_rows;
    const int prev_on_screen  = std::min(prev_rows, term_h);
    const int updatable_start = prev_rows - prev_on_screen;
    const int common          = std::min(content_rows, prev_rows);

    // Locate the first row that actually changed within the on-screen
    // portion of the overlap. Rows in scrollback (y < updatable_start)
    // can't be updated, so we skip them entirely.
    int first_changed = common;
    if (prev_rows > 0 && static_cast<std::size_t>(prev_rows) * static_cast<std::size_t>(W)
                        <= state.prev_cells.size())
    {
        const uint64_t* prev = state.prev_cells.data();
        for (int y = updatable_start; y < common; ++y) {
            if (!simd::bulk_eq(cells + y * W, prev + y * W,
                               static_cast<std::size_t>(W)))
            {
                first_changed = y;
                break;
            }
        }
    }

    // Nothing to do: the common range is identical and no rows were
    // added or removed.
    if (first_changed == common && content_rows == prev_rows) return;

    out += ansi::sync_start;
    out += ansi::hide_cursor;

    if (prev_rows == 0) {
        // First render from the caller's cursor position (assumed col 0).
        serialize(canvas, pool, out, content_rows);
    } else {
        // Position the cursor at column 0 of `first_changed`. The cursor
        // is currently at (prev_rows - 1, somewhere on that row).
        const int cursor_row = prev_rows - 1;
        const int delta      = first_changed - cursor_row;

        if (delta < 0) {
            int up = std::min(-delta, prev_on_screen - 1);
            if (up > 0) ansi::write_cursor_up(out, up);
            out += '\r';
        } else if (delta == 0) {
            out += '\r';
        } else {
            // Growing past the previous bottom — step down delta rows.
            // The first \r\n scrolls the terminal if we were already at
            // the last line, which is exactly what inline mode wants.
            out += "\r\n";
            if (delta > 1) ansi::write_cursor_down(out, delta - 1);
        }

        // Rewrite rows [first_changed, content_rows). serialize() emits
        // \r\n between rows and \x1b[K after each, so stale tail content
        // from the previous frame is cleaned up automatically.
        serialize(canvas, pool, out, content_rows, first_changed);

        // Content shrank — erase any leftover rows that are still on-screen.
        // Rows that already rolled into scrollback are immutable.
        if (content_rows < prev_rows) {
            int extra = std::min(prev_rows - content_rows, prev_on_screen);
            for (int i = 0; i < extra; ++i)
                out += "\r\n\x1b[2K";
            if (extra > 0) ansi::write_cursor_up(out, extra);
        }
    }

    // Don't emit show_cursor here — the in-frame hide_cursor above is for
    // *flicker* prevention during the redraw, NOT a hint that the cursor
    // should be re-shown between frames. Restoring it every tick reverses
    // any startup hide_cursor the app sent (e.g. moha's inline-mode setup),
    // and a TUI that wants a visible cursor can paint its own caret glyph.
    // Final cursor visibility is set once at startup and once at cleanup.
    out += ansi::sync_end;

    // Commit: cache the new cell buffer for next frame's comparison.
    const std::size_t new_size =
        static_cast<std::size_t>(content_rows) * static_cast<std::size_t>(W);
    if (state.prev_cells.size() < new_size)
        state.prev_cells.resize(new_size);
    std::memcpy(state.prev_cells.data(), cells, new_size * sizeof(uint64_t));
    state.prev_width = W;
    state.prev_rows  = content_rows;
}

} // namespace maya
