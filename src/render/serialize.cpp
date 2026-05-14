#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8
// simd::bulk_eq / find_first_diff come from canvas.hpp → core/simd.hpp
#include "maya/terminal/ansi.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace maya {

void InlineFrameState::commit_prefix(int rows) noexcept {
    // Bounds: clamp to [0, prev_rows].  Negative / zero is a no-op; an
    // over-commit (rows >= prev_rows) is interpreted as "everything is
    // scrollback now" and resets the state cleanly — no UB, no
    // out-of-bounds memmove on the caller.  This is the contract the
    // application can rely on regardless of how it tracks row counts.
    if (rows <= 0 || prev_rows <= 0 || prev_width <= 0) return;
    if (rows >= prev_rows) { reset(); return; }

    // Both `shift` and `remaining` are products of int * int → size_t.
    // The rows < prev_rows guard above plus the prev_rows * W ≤
    // prev_cells.size() invariant established by compose_inline_frame
    // mean the arithmetic cannot overflow on valid state — but we still
    // bound-check the memmove against actual buffer size in case the
    // application reset prev_cells externally between frames.
    const std::size_t W = static_cast<std::size_t>(prev_width);
    const std::size_t shift = static_cast<std::size_t>(rows) * W;
    const std::size_t remaining = static_cast<std::size_t>(prev_rows - rows) * W;

    uint64_t* data = prev_cells.data();
    if (data != nullptr && shift + remaining <= prev_cells.size()) {
        std::memmove(data, data + shift, remaining * sizeof(uint64_t));
    }
    prev_rows -= rows;

    // Cursor invariant: the next compose_inline_frame call assumes the
    // terminal cursor is at row (prev_rows - 1) in the post-commit
    // numbering.  That holds here because the caller (Runtime::
    // commit_inline_prefix) only mutates the renderer's mental model;
    // the actual terminal cursor is wherever the last write left it,
    // and the relative cursor moves used by compose_inline_frame
    // (cursor_up / \r\n) target rows by their distance from the
    // current cursor row, not absolute coordinates.  As long as the
    // application calls commit_prefix BEFORE the next render(), the
    // delta math is consistent.
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
            // Differential SGR — see StylePool::write_transition_sgr.
            // Saves bytes per transition by skipping the redundant
            // `0;` reset when previous state is known.
            flush_ascii();
            pool.write_transition_sgr(current_style, sid, out);
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
//
// Callers that have access to the Canvas should use canvas.last_content_col(y)
// instead — that's O(1) (incrementally maintained by set()/fill()/write_text)
// vs this function's O(W) backward scan. The function is retained for the
// inline diff path where the comparison buffer is a raw uint64_t* with no
// owning Canvas (state.prev_cells from a previous frame).
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
        // background color, inverse, etc. last_content_col() is O(1) —
        // canvas maintains it incrementally.
        const int last_col = canvas.last_content_col(y);
        if (last_col >= 0) {
            emit_cell_run(canvas, pool, y, 0, last_col + 1, current_style, out);
        }
        // EL 0 cleans up any stale content from a prior frame whose row
        // was wider than the current one. Reset SGR first so the erased
        // cells don't inherit attributes from the last emitted cell —
        // particularly underline / inverse / bg-color, which most
        // modern terminals (alacritty, kitty, vte-based, iTerm 3.5+)
        // apply to EL'd cells per the spec. Without this, a row whose
        // last cell is styled (e.g. a [link](url) at row end) would
        // visually extend its underline to end-of-row.
        if (current_style != 0) {
            out.append(pool.sgr(0));
            current_style = 0;
        }
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
// Bounded multiplication for content_rows × width.  Returns SIZE_MAX on
// overflow — the caller treats that as "drop the prev cache, do a full
// repaint" rather than silently corrupting prev_cells with a wrapped
// size that re-uses uninitialized capacity.
[[nodiscard]] static constexpr std::size_t safe_cells(int rows, int W) noexcept {
    if (rows <= 0 || W <= 0) return 0;
    const auto r = static_cast<std::size_t>(rows);
    const auto w = static_cast<std::size_t>(W);
    if (r > (std::size_t)(-1) / w) return (std::size_t)(-1);
    return r * w;
}

// Maximum frames `compose_inline_frame` may hold a small diff before
// forcing emission. Two consecutive holds means the worst-case
// accumulated diff is bounded at 2*min_changed_rows rows — still small
// enough that the eventual flush is one cheap frame, not a wall.
static constexpr int kMaxConsecutiveHolds = 2;

void compose_inline_frame(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState& state,
                          std::string& out,
                          bool synchronized_output,
                          int min_changed_rows)
{
    const int W = canvas.width();
    if (W <= 0 || content_rows <= 0 || term_h <= 0) return;

    // Width change invalidates the cached cell buffer — row layouts shift.
    // Reset clears prev_width / prev_rows but doesn't free prev_cells; the
    // next resize will reuse the allocation if it's already big enough.
    if (state.prev_width != W) state.reset();

    // Pre-reserve `out` so the per-row ANSI emission doesn't trip a
    // reallocation cascade.  Rough heuristic: 24 bytes per row of pure
    // movement + EL + style switches, plus ~200 bytes for the frame
    // wrapper.  Streaming workloads rarely write more than a few rows
    // per frame, so this dominates the actual byte count and saves
    // 2-3 reallocations on the hot path.
    out.reserve(out.size() + 256 + static_cast<std::size_t>(content_rows) * 24);

    const uint64_t* cells = canvas.cells();
    const int prev_rows       = state.prev_rows;
    const int prev_on_screen  = std::min(prev_rows, term_h);
    const int updatable_start = prev_rows - prev_on_screen;
    const int common          = std::min(content_rows, prev_rows);

    const std::size_t need_prev = safe_cells(prev_rows, W);
    const bool have_prev =
        prev_rows > 0 && need_prev != (std::size_t)(-1) &&
        need_prev <= state.prev_cells.size();
    const uint64_t* prev = have_prev ? state.prev_cells.data() : nullptr;

    // Locate the first row that actually differs from the cached copy
    // AND (when coalescing is requested) count the total changed-row
    // count. Rows in scrollback (y < updatable_start) are immutable —
    // skip them. The full walk is only done when the coalesce knob is
    // active; otherwise we bail at the first hit as before.
    int first_changed = common;
    int changed_rows  = 0;
    if (have_prev) {
        for (int y = updatable_start; y < common; ++y) {
            if (!simd::bulk_eq(cells + y * W, prev + y * W,
                               static_cast<std::size_t>(W)))
            {
                if (first_changed == common) first_changed = y;
                ++changed_rows;
                if (min_changed_rows <= 0) break;
            }
        }
    }

    // Nothing to do: common range matches and no rows added or removed.
    if (first_changed == common && content_rows == prev_rows) {
        state.held_count = 0;
        return;
    }

    // Bandwidth coalesce: when the diff touches few rows AND the frame
    // didn't grow/shrink, hold this emit and let the next frame's diff
    // accumulate against the same prev_cells. State is NOT mutated — the
    // next call's bulk_eq walk runs against the same baseline, so the
    // accumulated change strictly contains this frame's change.
    if (min_changed_rows > 0
        && content_rows == prev_rows
        && changed_rows > 0
        && changed_rows <= min_changed_rows
        && state.held_count < kMaxConsecutiveHolds)
    {
        ++state.held_count;
        return;
    }
    state.held_count = 0;

    // ── Frame open ─────────────────────────────────────────────────────
    // hide_cursor and DECAWM-off are emitted once and persisted across
    // frames (see InlineFrameState::{cursor_hidden, decawm_off}). Saves
    // ~16 bytes per frame on slow ttys where every escape costs RTT or
    // glyph-cache work. State.finalize() restores both on shutdown.
    if (synchronized_output) out += ansi::sync_start;
    if (!state.cursor_hidden) {
        out += ansi::hide_cursor;
        state.cursor_hidden = true;
    }

    // First-ever render (prev_rows == 0). Two distinct sub-cases,
    // differentiated by `state.prev_width`:
    //
    //   (A) prev_width == 0: truly fresh state — startup, or after a
    //       Divergent → Synced transition where the host emitted
    //       \x1b[2J\x1b[3J\x1b[H to clear+home. Cursor is at host's
    //       current position (row 0 after a home, or wherever the
    //       shell left it at startup). Emit serialize() from that
    //       position, growing the frame downward via \r\n's that
    //       scroll the terminal at term_h - 1. Live frame appears
    //       AT the cursor, host content above stays visible (inline
    //       mode convention).
    //
    //   (B) prev_width > 0: a previous frame was emitted and is on
    //       the wire, but force_redraw zeroed prev_rows to signal
    //       "diff state is stale, redraw fresh". Cursor was left at
    //       the previous frame's last row by that last emit (could
    //       be mid-viewport if a shrink had moved it up). Do an
    //       in-place soft redraw: cursor_up(content_rows - 1) to
    //       move up to where the frame's top should land (clamping
    //       at row 0 if it would overshoot), \r, serialize, then
    //       \x1b[J to clear anything below. For the typical
    //       force_redraw case (cursor at viewport row K, content
    //       fits above), cursor lands at K - content_rows + 1,
    //       serialize emits without scrolling, cursor returns to K.
    //       Composer stays at its viewport row — no "rushes to
    //       bottom" jolt, scrollback preserved.
    if (prev_rows == 0) {
        if (state.prev_width > 0) {
            // Force_redraw case (B): in-place soft redraw.
            //
            // Cap the emit at the visible viewport (last term_h rows of
            // the canvas) rather than the full content_rows. The frame
            // already extends past viewport top — the tail above the
            // viewport sits in native scrollback exactly as the stream
            // originally committed it, and emitting it again would only
            // (a) push the user's prior viewport content into scrollback
            // via the bottom-edge \r\n scrolls serialize uses to walk
            // rows, and (b) leave a duplicate copy of the just-finished
            // turn one screen above the live one — the "everything gets
            // re-printed from the beginning of the turn" symptom.
            //
            // The visible portion alone is enough to wipe any ghost
            // cells in the viewport (the original reason force_redraw
            // exists: composer outlines that survived a stream-finish
            // shrink because the shrink only cleared rows past the new
            // bottom, not stale composer cells already in mid-frame).
            // Scrollback ghosts can't be touched by any inline-mode
            // emit anyway — they're committed.
            const int up = std::min(content_rows - 1,
                                    std::max(0, term_h - 1));
            if (up > 0) ansi::write_cursor_up(out, up);
            out += '\r';
            const int start_row = std::max(0, content_rows - term_h);
            serialize(canvas, pool, out, content_rows, start_row);
            out += "\x1b[J";
        } else {
            // Fresh state (A): inline-mode growth from cursor.
            serialize(canvas, pool, out, content_rows);
        }
        if (synchronized_output) out += ansi::sync_end;
        // Cache the new cell buffer for next frame's comparison.  This is
        // the one path that legitimately needs a full memcpy because
        // prev_cells is empty.  Overflow check protects against pathological
        // inputs (e.g. content_rows = INT_MAX from a buggy auto_height).
        const std::size_t new_size = safe_cells(content_rows, W);
        if (new_size == (std::size_t)(-1)) {
            state.reset();   // prev_cells unchanged; caller falls back to Divergent next frame
            return;
        }
        if (state.prev_cells.size() < new_size) state.prev_cells.resize(new_size);
        std::memcpy(state.prev_cells.data(), cells, new_size * sizeof(uint64_t));
        state.prev_width = W;
        state.prev_rows  = content_rows;
        return;
    }

    // A1 shadow-of-wire: pre-resize prev_cells now (before the per-row
    // loop writes to it). Cells we don't touch are guaranteed to match
    // canvas by the inductive invariant (rows < first_changed haven't
    // changed; cells outside [x_first_diff, x_last_diff] in a changed
    // row haven't changed; new rows get a full-row copy in the loop).
    const std::size_t new_total_pre = safe_cells(content_rows, W);
    if (new_total_pre == (std::size_t)(-1)) {
        // Pathological size — drop the cache, fall back to Divergent
        // next frame.
        state.reset();
        return;
    }
    if (state.prev_cells.size() < new_total_pre)
        state.prev_cells.resize(new_total_pre);
    // Resize may have moved storage; re-take the pointer. `prev` is
    // const because the diff scan reads from it; we use a separate
    // mutable handle for the shadow writes.
    prev          = state.prev_cells.data();
    uint64_t* prev_w = state.prev_cells.data();

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
    // Persisted across frames via state.decawm_off; only the first frame
    // (or first after a state.reset()) pays the 5-byte escape.
    if (!state.decawm_off) {
        out += "\x1b[?7l";
        state.decawm_off = true;
    }
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

        if (x_first_diff < W) {
            const int x_last_diff    = last_diff_col(cur_row, prev_row, W);
            const int x_last_visible = canvas.last_content_col(y);

            // Emit cells through the last *visible* differing column.
            // If the tail of the diff is "current row went blank where
            // prev had content", we don't print blanks — we let EL
            // clean it up below.
            const int x_end_emit = std::max(x_first_diff,
                                            std::min(x_last_diff + 1,
                                                     x_last_visible + 1));

            // Cursor is at col 0 (we just emitted \r or \r\n at row
            // start). We move it forward to x_first_diff exactly once
            // — for either the cell-emit path or the EL-only path —
            // so we never overwrite unchanged cells at the start of
            // the row.
            const bool need_el   = is_new_row || x_last_diff > x_last_visible;
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
                // EL erases from cursor to end of line, preserving
                // cells before the cursor. Reset SGR before EL so the
                // erased region inherits no attributes from the last
                // emitted cell.
                if (current_style != 0) {
                    out.append(pool.sgr(0));
                    current_style = 0;
                }
                out += "\x1b[K";
            }

            // A1 shadow-of-wire: update prev_cells for the cells the
            // wire now shows. For a common-range row, only
            // [x_first_diff, x_last_diff+1) can differ; cells outside
            // that range matched prev by the definition of
            // first_diff_col / last_diff_col. For a new row prev_cells
            // doesn't have valid content yet, so copy the whole row.
            if (is_new_row) {
                std::memcpy(prev_w + (std::size_t)y * W,
                            cur_row,
                            (std::size_t)W * sizeof(uint64_t));
            } else {
                const std::size_t lo = (std::size_t)x_first_diff;
                const std::size_t hi = (std::size_t)x_last_diff + 1;
                std::memcpy(prev_w + (std::size_t)y * W + lo,
                            cur_row + lo,
                            (hi - lo) * sizeof(uint64_t));
            }
        } else if (is_new_row) {
            // All-blank new row — prev_cells has no entry yet. Copy the
            // (blank-packed) row so next frame's bulk_eq sees
            // blank-vs-blank match instead of zero-vs-default_cell.
            std::memcpy(prev_w + (std::size_t)y * W,
                        cur_row,
                        (std::size_t)W * sizeof(uint64_t));
        }
        // else: common-range identical row — prev_cells already
        // matches by induction; nothing to update.
    }

    // DECAWM is NOT restored here — it persists off across frames via
    // state.decawm_off. The next frame's diff path skips re-emitting
    // \x1b[?7l because state.decawm_off is already true. On shutdown
    // (or width change → state.reset()) the owner calls
    // state.finalize(out) to restore.
    out += ansi::reset;   // drop residual SGR

    // ── Shrink: erase rows past the new content_rows ───────────────────
    // Cursor is at row last_row_to_visit. Step into the abandoned region
    // and clear each leftover line, then pop back up so the next frame's
    // cursor-row assumption (prev_rows - 1 = content_rows - 1) holds.
    // Note: ansi::reset is emitted just above (line 408 in this function),
    // so SGR is already at default by the time we get here — \e[2K
    // erases unstyled rows correctly without an additional reset.
    if (content_rows < prev_rows) {
        // Clear the abandoned region with a single ED (erase-in-display)
        // from cursor to end of screen. The previous implementation
        // walked the region with `\r\n\x1b[2K` per row, which at viewport
        // bottom turned each `\r\n` into a scroll — committing the
        // top-of-viewport row to native scrollback as a duplicate of
        // rows the stream's natural overflow had already put there.
        // The per-row cap attempt couldn't see the cursor's terminal
        // row when first_changed > 0 (cursor lands mid-viewport, not
        // at content_rows - 1), so it under-cleared and left old
        // composer / status rows visible below the new frame.
        //
        // \x1b[J wipes from the cursor's current column through end of
        // the cursor's row and every row below it inside the viewport;
        // it never moves the cursor and never scrolls. Off-viewport
        // abandoned rows don't exist on the terminal anyway. After
        // this the cursor is exactly where the per-row loop left it —
        // at the new content_rows - 1 — so the next frame's cursor-row
        // assumption (prev_rows - 1 = content_rows - 1) holds without
        // a cursor_up pop.
        out += "\x1b[J";
    }

    if (synchronized_output) out += ansi::sync_end;

    // ── Commit: prev_cells is already up-to-date ───────────────────────
    //
    // A1 shadow-of-wire: the per-row loop wrote each emitted slice into
    // prev_cells as it ran, so the bulk end-of-function memcpy that
    // older versions did here is no longer needed. Rows in
    // [0, first_changed) were byte-identical to begin with and are
    // untouched. Rows in [first_changed, content_rows) had their
    // canvas-visible portion copied into prev_cells inside the loop.
    // The buffer was pre-resized before the loop started, so writes
    // always landed in valid storage.
    //
    // Correctness: next frame's bulk_eq walks prev_cells[y][0..W]
    // against canvas[y][0..W] for y < min(content_rows, prev_rows).
    // By induction every cell in that range either matched last frame
    // (untouched here) or was just written from canvas (this frame's
    // shadow update) — so prev_cells equals the wire content.
    //
    // Perf: instead of O((content_rows - first_changed) × W × 8) bytes
    // memcpy'd unconditionally, the actual cost is the sum of the
    // changed-slice widths per visited row plus a full-row copy for
    // new rows. Sparse streaming frames (one line in a long transcript
    // changing) drop from "copy every row below first_changed" to
    // "copy a few hundred bytes per changed row".
    state.prev_width = W;
    state.prev_rows  = content_rows;
}

} // namespace maya
