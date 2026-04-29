#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8
// simd::bulk_eq / find_first_diff come from canvas.hpp → core/simd.hpp
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

namespace {

// ──────────────────────────────────────────────────────────────────────
// emit_cell_run — write cells [x_begin, x_end) of row y into `out`.
//
// Threads SGR state through `current_style` so consecutive runs in the
// same frame don't re-emit identical CSI sequences. Caller owns:
//   - cursor positioning (assumes cursor already at (y, x_begin))
//   - row separator (\r\n) before/after if applicable
//   - DECAWM bracket
//   - EL after this run if needed to clear stale tail
//
// This is the smallest reusable unit: full-row serialize() loops over
// it once per row; the inline diff calls it per changed sub-span.
// ──────────────────────────────────────────────────────────────────────
void emit_cell_run(const Canvas& canvas, const StylePool& pool,
                   int y, int x_begin, int x_end,
                   uint16_t& current_style, std::string& out)
{
    if (x_begin >= x_end) return;
    const uint64_t* cells = canvas.cells();
    const int row_base = y * canvas.width();

    char ascii_buf[256];
    int ascii_len = 0;
    auto flush_ascii = [&] {
        if (ascii_len > 0) {
            out.append(ascii_buf, static_cast<size_t>(ascii_len));
            ascii_len = 0;
        }
    };

    for (int x = x_begin; x < x_end; ++x) {
        const uint64_t packed = cells[row_base + x];
        const auto ch  = static_cast<char32_t>(packed & 0xFFFFFFFF);
        const auto sid = static_cast<uint16_t>((packed >> 32) & 0xFFFF);
        const auto w   = static_cast<uint8_t>(packed >> 56);

        if (w == 2) [[unlikely]] continue; // wide-char second half placeholder

        if (sid != current_style) [[unlikely]] {
            flush_ascii();
            out.append(pool.sgr(sid));
            current_style = sid;
        }

        if (ch < 0x80) [[likely]] {
            ascii_buf[ascii_len++] = static_cast<char>(ch);
            if (ascii_len == 256) [[unlikely]] flush_ascii();
        } else {
            flush_ascii();
            detail::encode_utf8(ch, out);
        }
    }
    flush_ascii();
}

// Find the last column of row y whose cell carries visible content
// (non-blank glyph or any styling). Returns -1 if the row is entirely
// blank+unstyled. Mirrors the trim logic used by serialize().
[[nodiscard]] int last_visible_col(const uint64_t* row, int W) noexcept {
    for (int x = W - 1; x >= 0; --x) {
        const uint64_t p = row[x];
        const auto c = static_cast<char32_t>(p & 0xFFFFFFFF);
        const auto s = static_cast<uint16_t>((p >> 32) & 0xFFFF);
        if ((c != U' ' && c != 0) || s != 0) return x;
    }
    return -1;
}

// Backward scan for the last column where two row buffers differ.
// `cur` is always valid; `prev` may be null, in which case the prev
// row is treated as logically blank (Cell{}.pack()). Returns -1 if no
// difference exists in [0, W).
[[nodiscard]] int last_diff_col(const uint64_t* cur, const uint64_t* prev,
                                int W) noexcept {
    static const uint64_t blank = Cell{}.pack();
    for (int x = W - 1; x >= 0; --x) {
        const uint64_t p = prev ? prev[x] : blank;
        if (cur[x] != p) return x;
    }
    return -1;
}

// Equivalent of simd::find_first_diff but tolerant of a null `prev`
// (treats it as a buffer of blank cells without materialising one).
[[nodiscard]] int first_diff_col(const uint64_t* cur, const uint64_t* prev,
                                 int W) noexcept {
    if (prev) {
        return static_cast<int>(simd::find_first_diff(
            cur, prev, static_cast<std::size_t>(W)));
    }
    static const uint64_t blank = Cell{}.pack();
    for (int x = 0; x < W; ++x) if (cur[x] != blank) return x;
    return W;
}

// Write CSI <n> C (cursor forward) for n > 0. n == 0 is a no-op.
void write_cursor_forward(std::string& out, int n) {
    if (n <= 0) return;
    out += "\x1b[";
    ansi::detail::append_int(out, n);
    out += 'C';
}

} // namespace

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
    // the right margin don't wrap to the next line. Re-enabled at the end
    // of this function — every early return must restore it.
    out += "\x1b[?7l";

    for (int y = y_begin; y < y_end; ++y) {
        if (y > y_begin) out += "\r\n";

        // Trim trailing blanks: only emit through the last visible cell.
        // Styled spaces (sid != 0) are treated as content — they may carry
        // background color, inverse, etc.
        const int last_col = last_visible_col(cells + y * W, W);
        if (last_col >= 0) {
            emit_cell_run(canvas, pool, y, 0, last_col + 1, current_style, out);
        }
        // EL 0 cleans up any stale content from a prior frame whose row
        // was wider than the current one.
        out += "\x1b[K";
    }

    out += "\x1b[?7h";   // re-enable auto-wrap
    out += ansi::reset;  // reset SGR at end of frame
}

// ============================================================================
// compose_inline_frame
// ============================================================================
//
// Inline-mode incremental update. The output is a sequence of cursor
// moves and per-row sub-span rewrites: only the cells that actually
// changed between the previous and current canvas are emitted.
//
// Compared to the older "find first changed row, redraw the contiguous
// tail" approach, this matters in two stacked cases that are the norm
// during streaming:
//
//   1. The bottom of the frame animates every tick (spinner / status
//      bar / sparkline) even when the rest is idle.
//   2. New tokens land in mid-frame, growing a message bubble by one
//      wrapped row, while the spinner above-keeps-ticking.
//
// In both cases the old algorithm rewrote every row from the topmost
// change down to the bottom — including the unchanged middle. On a
// terminal that doesn't honor DEC mode 2026 (synchronized output), that
// large, contiguous repaint is rendered byte-by-byte and shows as
// flicker. The per-row + per-cell-span path emits only the rows that
// actually differ, and within each row only the changed columns —
// typically an order-of-magnitude reduction on streaming workloads.
//
// `synchronized_output` toggles the DEC 2026 wrapper: pass true when
// the host terminal is known to honor it (modern Kitty, WezTerm,
// Ghostty, Windows Terminal, Alacritty, iTerm 3.5+, vte ≥ 6200) so
// the frame swaps atomically. Pass false to skip the harmless-but-
// pointless escape on terminals that ignore it (Apple Terminal, plain
// xterm). The bytes save nothing visible either way, but knowing the
// answer upstream lets the application coalesce paints more
// aggressively when sync isn't available.
void compose_inline_frame(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState& state,
                          std::string& out,
                          bool synchronized_output)
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

    const bool have_prev =
        prev_rows > 0 &&
        static_cast<std::size_t>(prev_rows) * static_cast<std::size_t>(W)
            <= state.prev_cells.size();
    const uint64_t* prev = have_prev ? state.prev_cells.data() : nullptr;

    // Locate the first row that actually differs from the cached copy.
    // Rows in scrollback (y < updatable_start) are immutable — skip them.
    int first_changed = common;
    if (have_prev) {
        for (int y = updatable_start; y < common; ++y) {
            if (!simd::bulk_eq(cells + y * W, prev + y * W,
                               static_cast<std::size_t>(W)))
            {
                first_changed = y;
                break;
            }
        }
    }

    // Nothing to do: common range matches and no rows added or removed.
    if (first_changed == common && content_rows == prev_rows) return;

    // ── Frame open ─────────────────────────────────────────────────────
    if (synchronized_output) out += ansi::sync_start;
    out += ansi::hide_cursor;

    // First-ever render: no cursor positioning needed; serialize() handles
    // DECAWM, EL, and row separators on its own.
    if (prev_rows == 0) {
        serialize(canvas, pool, out, content_rows);
        if (synchronized_output) out += ansi::sync_end;
        // Cache the new cell buffer for next frame's comparison.
        const std::size_t new_size =
            static_cast<std::size_t>(content_rows) * static_cast<std::size_t>(W);
        if (state.prev_cells.size() < new_size) state.prev_cells.resize(new_size);
        std::memcpy(state.prev_cells.data(), cells, new_size * sizeof(uint64_t));
        state.prev_width = W;
        state.prev_rows  = content_rows;
        return;
    }

    // ── Position cursor at row first_changed, col 0 ────────────────────
    // Cursor is currently at the last row of the previously-rendered
    // frame (prev_rows - 1) at some column inside the content. Move
    // relatively: cursor_up for backward, \r\n for forward (the latter
    // scrolls the terminal at the bottom edge, which is exactly what
    // inline mode wants when growing past the previous bottom).
    const int cursor_row_start = prev_rows - 1;
    {
        const int delta = first_changed - cursor_row_start;
        if (delta < 0) {
            int up = std::min(-delta, prev_on_screen - 1);
            if (up > 0) ansi::write_cursor_up(out, up);
            out += '\r';
        } else if (delta == 0) {
            out += '\r';
        } else {
            // Growing past previous bottom — first \r\n scrolls the
            // terminal, then cursor_down advances within the new region.
            out += "\r\n";
            if (delta > 1) ansi::write_cursor_down(out, delta - 1);
        }
    }

    // DECAWM off for the entire frame body — emitted once, not per row.
    out += "\x1b[?7l";
    uint16_t current_style = UINT16_MAX;

    // ── Per-row, per-cell-span emission ────────────────────────────────
    // Iterate from first_changed down through max(content_rows-1,
    // last_changed_in_common). Unchanged rows in the middle just advance
    // the cursor (\r\n) without emitting any cell content.
    const int last_row_to_visit = content_rows - 1;
    for (int y = first_changed; y <= last_row_to_visit; ++y) {
        if (y > first_changed) out += "\r\n";  // advance to row y, col 0

        const uint64_t* cur_row  = cells + y * W;
        const uint64_t* prev_row = (have_prev && y < prev_rows)
                                 ? prev + y * W
                                 : nullptr;
        const bool is_new_row    = (y >= prev_rows);

        // Find the changed sub-span. For new rows (no prev), this is
        // [first_non_blank, last_non_blank+1).
        const int x_first_diff = first_diff_col(cur_row, prev_row, W);
        if (x_first_diff >= W) continue; // row is identical — no emission

        const int x_last_diff   = last_diff_col(cur_row, prev_row, W);
        const int x_last_visible = last_visible_col(cur_row, W);

        // Emit cells through the last *visible* differing column. If the
        // tail of the diff is "current row went blank where prev had
        // content", we don't print blanks — we let EL clean it up below.
        const int x_end_emit = std::max(x_first_diff,
                                        std::min(x_last_diff + 1,
                                                 x_last_visible + 1));

        // Cursor is at col 0 (we just emitted \r or \r\n at row start).
        // We move it forward to x_first_diff exactly once — for either
        // the cell-emit path or the EL-only path — so we never overwrite
        // unchanged cells at the start of the row.
        const bool need_el = is_new_row || x_last_diff > x_last_visible;
        const bool need_emit = x_end_emit > x_first_diff;

        if (need_emit || need_el) {
            write_cursor_forward(out, x_first_diff);
        }
        if (need_emit) {
            emit_cell_run(canvas, pool, y, x_first_diff, x_end_emit,
                          current_style, out);
            // cursor now at col x_end_emit
        }
        if (need_el) {
            // EL erases from cursor to end of line, preserving cells before
            // the cursor. After need_emit the cursor sits at x_end_emit
            // (≥ first non-blank tail); without need_emit it's at
            // x_first_diff (which is past x_last_visible in the trailing-
            // blank-only diff case, so visible content stays intact).
            out += "\x1b[K";
        }
    }

    out += "\x1b[?7h";    // restore DECAWM
    out += ansi::reset;   // drop residual SGR

    // ── Shrink: erase rows past the new content_rows ───────────────────
    // Cursor is at row last_row_to_visit. Step into the abandoned region
    // and clear each leftover line, then pop back up so the next frame's
    // cursor-row assumption (prev_rows - 1 = content_rows - 1) holds.
    if (content_rows < prev_rows) {
        int extra = std::min(prev_rows - content_rows, prev_on_screen);
        for (int i = 0; i < extra; ++i) out += "\r\n\x1b[2K";
        if (extra > 0) ansi::write_cursor_up(out, extra);
    }

    if (synchronized_output) out += ansi::sync_end;

    // Commit: cache the new cell buffer for next frame's comparison.
    const std::size_t new_size =
        static_cast<std::size_t>(content_rows) * static_cast<std::size_t>(W);
    if (state.prev_cells.size() < new_size) state.prev_cells.resize(new_size);
    std::memcpy(state.prev_cells.data(), cells, new_size * sizeof(uint64_t));
    state.prev_width = W;
    state.prev_rows  = content_rows;
}

} // namespace maya
