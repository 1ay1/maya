#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8
// simd::bulk_eq / find_first_diff come from canvas.hpp → core/simd.hpp
#include "maya/terminal/ansi.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#ifdef MAYA_DEBUG_SHADOW_VERIFY
#  include <cstdio>
#  include <cstdlib>
#endif

namespace maya {

TermRows query_term_rows(platform::NativeHandle handle) noexcept {
    const auto sz = platform::query_terminal_size(handle);
    const int h = sz.height.raw();
    return TermRows{h > 0 ? h : 24};
}

// ShadowWitness producer. Folds prev_cells with FNV-1a and compares
// to the value compose stored at the end of the prior frame,
// returning either a populated optional carrying the witness (the
// hash matched, or the state was fresh) or nullopt (hash mismatch —
// the shadow is poisoned).
//
// Note we hash even on the "fresh" path so the witness carries a
// concrete value, not the UINT64_MAX sentinel. That way the
// re-verification done by compose_inline_frame on consumption is
// always comparing against a real folded hash, never against the
// sentinel.
std::optional<ShadowWitness> verify_shadow(const InlineFrameState& state) noexcept {
    // Fresh state: no prior shadow exists, so trivially "matches".
    // Witness carries the current empty-state hash.
    if (state.shadow_hash_ == static_cast<uint64_t>(-1) ||
        state.prev_width_ <= 0 || state.prev_rows_ <= 0) {
        return ShadowWitness{&state, 0ULL};
    }

    const std::size_t W = static_cast<std::size_t>(state.prev_width_);
    const std::size_t n = static_cast<std::size_t>(state.prev_rows_) * W;
    if (n > state.prev_cells_.size()) return std::nullopt;

    uint64_t h = 14695981039346656037ULL;
    const uint64_t* p = state.prev_cells_.data();
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    if (h != state.shadow_hash_) return std::nullopt;
    return ShadowWitness{&state, h};
}

FinalizeResult InlineFrameState::finalize() && noexcept {
    std::string out;
    if (cursor_hidden_) out.append("\x1b[?25h");
    if (decawm_off_)    out.append("\x1b[?7h");
    InlineFrameState s{std::move(*this)};
    s.cursor_hidden_ = false;
    s.decawm_off_    = false;
    return FinalizeResult{std::move(out), std::move(s)};
}

InlineFrameState InlineFrameState::committed(ScrollbackMarker marker) && noexcept {
    InlineFrameState s{std::move(*this)};
    const int rows = marker.rows();

    // Bounds: clamp to [0, prev_rows].  Negative / zero is a no-op;
    // an over-commit (rows >= prev_rows) is interpreted as
    // "everything is scrollback now" and returns a reset state.
    if (rows <= 0 || s.prev_rows_ <= 0 || s.prev_width_ <= 0) return s;
    if (rows >= s.prev_rows_) return std::move(s).reset_state();

    // Both `shift` and `remaining` are products of int * int → size_t.
    // The rows < prev_rows guard above plus the prev_rows * W ≤
    // prev_cells.size() invariant established by compose_inline_frame
    // mean the arithmetic cannot overflow on valid state — but we
    // still bound-check the memmove against actual buffer size in
    // case the application reset prev_cells externally between
    // frames.
    const std::size_t W = static_cast<std::size_t>(s.prev_width_);
    const std::size_t shift = static_cast<std::size_t>(rows) * W;
    const std::size_t remaining = static_cast<std::size_t>(s.prev_rows_ - rows) * W;

    uint64_t* data = s.prev_cells_.data();
    if (data != nullptr && shift + remaining <= s.prev_cells_.size()) {
        std::memmove(data, data + shift, remaining * sizeof(uint64_t));
    }
    s.prev_rows_ -= rows;

    // Re-hash the now-shifted prev_cells so the next render's
    // shadow-verify compares against the post-shift state instead
    // of false-positive on the legitimate scrollback advance.
    if (s.shadow_hash_ != static_cast<uint64_t>(-1)) {
        uint64_t h = 14695981039346656037ULL;
        const std::size_t n =
            static_cast<std::size_t>(s.prev_rows_) * W;
        for (std::size_t i = 0; i < n; ++i) {
            h ^= s.prev_cells_.data()[i];
            h *= 1099511628211ULL;
        }
        s.shadow_hash_ = h;
    }

    // Cursor invariant: the next compose_inline_frame call assumes
    // the terminal cursor is at row (prev_rows - 1) in the
    // post-commit numbering. That holds here because committing
    // only mutates the renderer's mental model; the actual terminal
    // cursor is wherever the last write left it, and the relative
    // cursor moves used by compose_inline_frame (cursor_up / \r\n)
    // target rows by their distance from the current cursor row,
    // not absolute coordinates. As long as the application commits
    // BEFORE the next render(), the delta math is consistent.
    return s;
}

int content_height(const Canvas& canvas) noexcept {
    // O(1): canvas tracks max_y_ during painting. -1 ⇒ nothing was
    // ever written this frame ⇒ zero rows of content.
    //
    // Returning 0 (not 1) is load-bearing for the inline path: the
    // run-loop's empty-frame guard (`if (ch <= 0) ...` in app.cpp
    // and inline.cpp) signals "no compose this tick, leave prev_rows
    // alone." An off-by-one return of 1 here used to slip past that
    // guard and cause compose_inline_frame to walk a single all-blank
    // row through its first-render path, momentarily writing a stray
    // line break into the host's terminal before the next real frame
    // overdrew it. The visible symptom on startup was a one-row
    // "hiccup" before the first turn appeared. Aligning the contract
    // with what the canvas actually holds eliminates the ambiguity.
    return canvas.max_content_row() + 1;
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

// Wide-glyph diff-edge snapping. The diff helpers above operate on raw
// packed u64 cells and have no notion of wide-char pairing. If the
// reported first/last diff column lands ON the trail half (width==2)
// of a wide glyph in EITHER cur or prev, emit_cell_run will silently
// `continue` over that trail and never repaint the lead — leaving the
// wire's old lead glyph in place while the shadow records the new
// canvas bytes as flushed. Snap the edge outward by one cell so the
// emit covers the entire wide pair.
//
// snap_first_diff_left:  if cur[x] OR prev[x] has width==2, return x-1
//                        (clamped to 0).
// snap_last_diff_right:  if cur[x] OR prev[x] has width==1 (lead),
//                        return x+1 (clamped to W-1).
//
// Both inputs (cur, prev) need to be checked: a layout shift can leave
// a stale wide pair in prev whose lead aligns with new content in cur,
// and vice versa.
[[nodiscard]] static inline uint8_t cell_width(uint64_t packed) noexcept {
    return static_cast<uint8_t>(packed >> 56);
}

[[nodiscard]] int snap_first_diff_left(int x, const uint64_t* cur,
                                       const uint64_t* prev, int W) noexcept {
    if (x <= 0 || x >= W) return x;
    const uint8_t cw = cell_width(cur[x]);
    const uint8_t pw = prev ? cell_width(prev[x]) : 0;
    if (cw == 2 || pw == 2) return x - 1;
    return x;
}

[[nodiscard]] int snap_last_diff_right(int x, const uint64_t* cur,
                                       const uint64_t* prev, int W) noexcept {
    if (x < 0 || x >= W - 1) return x;
    const uint8_t cw = cell_width(cur[x]);
    const uint8_t pw = prev ? cell_width(prev[x]) : 0;
    if (cw == 1 || pw == 1) return x + 1;
    return x;
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
        // Skip EL when the row filled through col W-1: DECAWM-off leaves
        // the cursor AT col W-1 (no advance past the right edge,
        // ECMA-48 §8.3.118), and \x1b[K from there would erase that
        // rightmost cell — the last-column corruption symptom on
        // full-width content (code-block right borders, full-width
        // rules). Nothing past W-1 exists to erase anyway.
        if (last_col < W - 1) {
            out += "\x1b[K";
        }
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

// Bandwidth coalesce (the `min_changed_rows` parameter that held a
// small diff across frames) was removed when the runtime concluded the
// hold was actively harmful on the typing path — a 1-row composer
// change would meet the hold criterion and swallow keystrokes until
// the third one forced a flush. See app.cpp's compose_inline_frame
// call site for the full diagnosis. The parameter, the per-state
// `held_count` field, and the early-exit suppression in the per-row
// diff scan all went with it. If a future renderer needs a similar
// optimisation, the design should live ON TOP of the simple emit (a
// caller-side decision to skip render entirely for a frame), not
// inside compose where it has visibility into row-level state but no
// way to tell whether the diff is user-input or animation.

// Internal byte-emitter for the inline frame path.
//
// This is the actual byte-producer that walks the canvas, diffs
// against `state.prev_cells`, and emits VT sequences. It is NOT a
// public symbol — the only legitimate caller is the witness-chain
// entrypoint `compose_inline_frame` in frame_bytes.cpp, which fuses
// this in-place mutation into the linear FrameBytes capsule so the
// state advance and byte delivery are inseparable.
//
// Direct callers cannot exist outside frame_bytes.cpp because the
// public surface for inline composition is the Witness Chain
// (`InlineFrame<Tag>::render`, which calls `compose_inline_frame`
// which calls THIS function). Bypassing the chain would mean
// composing without proving the shadow matches the wire — exactly
// the corruption surface the chain was built to close. The function
// is declared as a non-static symbol so it links across the TU
// boundary, but it has no public header declaration.
std::pair<std::string, InlineFrameState>
compose_inline_frame_impl(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState&& state_in,
                          bool synchronized_output)
{
    // Take ownership of the moved-in state. From here on `state` is
    // a function-local value; mutating it is unobservable to any
    // outside code (the caller gave up the value by &&-move). The
    // result is returned by value at the end — the operation is
    // mathematically a pure `(canvas, state_in) → (bytes, state_out)`
    // even though we write into the local for implementation
    // simplicity.
    InlineFrameState state{std::move(state_in)};
    std::string out;

    const int W = canvas.width();
    if (W <= 0 || content_rows <= 0 || term_h <= 0)
        return {std::move(out), std::move(state)};

#ifdef MAYA_DEBUG_SHADOW_VERIFY
    // Compose precondition: `content_rows` must equal
    // canvas.max_content_row() + 1 — i.e. the caller derived it from
    // content_height(canvas). If they don't match, somebody computed
    // content_rows from a different source (layout's computed height,
    // a hard-coded value, etc.) and the row-emit loop will walk past
    // the actual painted rows, serialising stale cells from previous
    // frames into the live frame. Catch the violation at the source
    // instead of letting it become a ghost-row report from a user.
    {
        const int expected = canvas.max_content_row() + 1;
        if (content_rows != expected) {
            std::fprintf(stderr,
                "\n[maya] COMPOSE PRECONDITION VIOLATED: content_rows=%d "
                "but canvas.max_content_row()+1=%d. The caller must derive "
                "content_rows from content_height(canvas).\n",
                content_rows, expected);
            std::abort();
        }
    }
#endif

    // Width change invalidates the cached cell buffer — row layouts shift.
    // Reset clears prev_width / prev_rows but doesn't free prev_cells; the
    // next resize will reuse the allocation if it's already big enough.
    if (state.prev_width_ != W) {
        state = std::move(state).reset_state();
        // Width-flap memory recovery: if the buffer is dramatically
        // larger than what this frame needs, release the excess so
        // a transient wide-window state can't pin memory forever.
        // shrink_to_fit_if_oversized's 2x threshold prevents
        // allocator thrashing on small steady-state variations.
        state.prev_cells_.shrink_to_fit_if_oversized(
            static_cast<std::size_t>(content_rows)
            * static_cast<std::size_t>(W));
    }

    // Pre-reserve `out` so the per-row ANSI emission doesn't trip a
    // reallocation cascade.  Rough heuristic: 24 bytes per row of pure
    // movement + EL + style switches, plus ~200 bytes for the frame
    // wrapper.  Streaming workloads rarely write more than a few rows
    // per frame, so this dominates the actual byte count and saves
    // 2-3 reallocations on the hot path.
    out.reserve(out.size() + 256 + static_cast<std::size_t>(content_rows) * 24);

    const uint64_t* cells = canvas.cells();
    const int prev_rows       = state.prev_rows_;
    const int prev_on_screen  = std::min(prev_rows, term_h);
    const int updatable_start = prev_rows - prev_on_screen;
    const int common          = std::min(content_rows, prev_rows);

    const std::size_t need_prev = safe_cells(prev_rows, W);
    const bool have_prev =
        prev_rows > 0 && need_prev != (std::size_t)(-1) &&
        need_prev <= state.prev_cells_.size();
    const uint64_t* prev = have_prev ? state.prev_cells_.data() : nullptr;

    // Locate the first row that actually differs from the cached copy.
    // Rows in scrollback (y < updatable_start) are immutable — skip
    // them. Early-exit on the first hit: the per-row emit below walks
    // from first_changed to content_rows-1 anyway, so the changed-rows
    // count beyond the first hit isn't useful here.
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

    // hide_cursor (DECTCEM ?25l) is RE-ASSERTED on every render call,
    // BEFORE the no-op early-return below, not latched. Inline mode
    // never positions the hardware cursor — the composer's caret is a
    // painted cell — so the real cursor must stay invisible for the
    // whole session, at ALL times including when the frame is idle.
    //
    // Anything OUTSIDE maya can re-show it between our frames: a
    // sandboxed subprocess that thinks it owns a tty and writes ?25h
    // to fd 1, an SSH reconnect, a terminal emulator that resets
    // DECTCEM on its own scroll/SU, or — the case that motivated
    // moving this above the early-return — a mobile terminal (Blink /
    // iSH / Termius on iOS) that shows its own cursor when the soft
    // keyboard or speech-to-text input field opens. If we only emit
    // the hide on content-changing frames, an idle composer (the
    // common state) hits the "nothing to do" return and never re-
    // hides, so the externally-reshown cursor sits there until the
    // next keystroke. Emitting the hide before the early-return
    // guarantees every render tick re-asserts invisibility regardless
    // of whether the canvas changed.
    //
    // ?25l is idempotent at the terminal (hiding an already-hidden
    // cursor is a no-op, paints nothing, moves nothing) so re-emitting
    // it on an otherwise-silent frame is safe: 6 bytes, no flicker. It
    // sits inside the sync wrapper on content frames; on the no-op
    // path it ships alone (no sync markers needed — there is no frame
    // body to make atomic).
    if (first_changed == common && content_rows == prev_rows) {
        out += ansi::hide_cursor;
        state.cursor_hidden_ = true;
        return {std::move(out), std::move(state)};
    }

    // ── Frame open ─────────────────────────────────────────────────────
    // DECAWM-off is emitted once and persisted across frames
    // (state.decawm_off_); state.finalize() restores it on shutdown.
    // hide_cursor is re-asserted here too (see the rationale at the
    // early-return above), inside the sync wrapper so it applies
    // atomically with the rest of the frame body.
    if (synchronized_output) out += ansi::sync_start;
    out += ansi::hide_cursor;
    state.cursor_hidden_ = true;

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
        // Audit finding #2 history: an earlier case-(B) implementation
        // emitted serialize() from canvas row 0 with a cursor_up capped
        // at term_h-1; when content_rows > term_h the bottom-edge
        // \r\n's scrolled native scrollback by (content_rows - term_h)
        // rows, leaving a ghost copy of the just-finished turn one
        // screen above the live one. That bug was originally patched
        // by routing oversized force_redraws through a hard
        // \x1b[2J\x1b[3J\x1b[H reset — which fixed the ghost but
        // wiped the host's pre-launch scrollback every time it fired.
        //
        // The current case-(B) emit shape (below) is already scrollback-
        // safe for oversized content: it caps the cursor_up at term_h-1
        // AND caps the serialize at start_row = max(0, content_rows -
        // term_h), so only the last term_h rows of canvas land in the
        // viewport via term_h-1 inter-row \r\n's — exactly filling the
        // existing viewport, zero bottom-edge scrolls, zero scrollback
        // mutation. Rows of the inline frame already above viewport top
        // stay in native scrollback as the stream originally committed
        // them (see the SCOPE CONTRACT comment in the path below).
        //
        // So: no oversized special-case here. The hard \x1b[2J\x1b[3J\x1b[H
        // reset is reserved for the Divergent path (resize / write-fail
        // recovery), where the layout assumptions actually changed and
        // a full repaint from home is the correct response — see
        // app.cpp's resize handler.
        if (state.prev_width_ > 0) {
            // Force_redraw case (B): in-place soft redraw.
            //
            // ── SCOPE CONTRACT ──────────────────────────────────────
            //
            // This path is a VIEWPORT-ONLY recovery. It repaints the
            // last `term_h` rows of the canvas in place; any rows of
            // the inline frame that already overflowed into the
            // terminal's native scrollback are NOT re-emitted, and
            // (in inline mode) CANNOT be re-emitted.
            //
            // Rationale: inline mode shares the terminal viewport
            // with the host's pre-existing scrollback (the shell
            // prompts and program output that lived above the
            // composer before agentty started). Any sequence capable
            // of overwriting committed scrollback rows would by
            // definition also overwrite the host's content — there
            // is no "my scrollback" vs "host's scrollback"
            // distinction at the VT level once a row scrolls off the
            // top edge. The cells are owned by the terminal
            // emulator and are immutable to the application.
            //
            // What this path DOES fix:
            //   • Ghost cells inside the live viewport — composer
            //     outline survivors of a stream-finish shrink, stale
            //     status / footer rows below the new content_rows,
            //     SGR residue from a half-written frame.
            //   • prev_cells / wire desync — caller (Runtime::force_redraw)
            //     zeroed prev_rows to mark "diff state is stale,
            //     redraw fresh", so the per-row diff skips its
            //     usual byte-identical fast paths and re-emits
            //     every visible row.
            //
            // What this path DOES NOT fix:
            //   • Scrollback corruption (a stray subprocess wrote
            //     directly to fd 1 mid-frame; a tmux pane swap
            //     mangled the rows above the viewport; a terminal
            //     emulator dropped bytes during a resize). Those
            //     rows are off-viewport, committed, and unreachable.
            //     The user-facing recovery for that case is the
            //     terminal emulator's own redraw (most emulators
            //     bind their own Ctrl-L to a full repaint of the
            //     emulator's local cell grid), or a resize event
            //     that drops agentty into the Divergent path (which
            //     DOES emit \x1b[2J\x1b[3J\x1b[H, wiping scrollback
            //     and starting fresh — deliberately reserved for
            //     resize / write-fail because the scrollback wipe
            //     is destructive to the host's prior output).
            //
            // Callers wiring a user-facing "redraw" hotkey to this
            // path (see agentty's RedrawScreen → Cmd::force_redraw
            // in update/meta.cpp) should mirror this scope in their
            // user-facing docs so users don't expect Ctrl-L to fix
            // every kind of terminal corruption.
            //
            // ── EMIT SHAPE ──────────────────────────────────────────
            //
            // Cap the emit at the visible viewport (last term_h rows
            // of the canvas) rather than the full content_rows. The
            // frame may extend past viewport top — the tail above the
            // viewport sits in native scrollback exactly as the
            // stream originally committed it, and emitting it again
            // would only (a) push the user's prior viewport content
            // into scrollback via the bottom-edge \r\n scrolls
            // serialize uses to walk rows, and (b) leave a duplicate
            // copy of the just-finished turn one screen above the
            // live one — the "everything gets re-printed from the
            // beginning of the turn" symptom.
            //
            // The viewport-cap is what makes this path scrollback-safe
            // for oversized content (content_rows > term_h) too:
            // start_row clamps to content_rows - term_h, the cursor_up
            // clamps to term_h - 1, serialize emits exactly term_h
            // rows with term_h - 1 inter-row \r\n's — fills the
            // viewport in place, no bottom-edge scroll.
            //
            // ── Shrink case: erase prior frame's top rows ──────────
            //
            // \x1b[J at the tail only erases from the cursor
            // downward. When the new frame is SHORTER than what
            // force_redraw zeroed (state.ghost_rows_above), the rows
            // above the new frame's top still hold the prior frame's
            // content and would stay visible. We pre-position the
            // cursor above the new frame's top, EL-erase one row,
            // and walk down — total erased rows =
            // min(ghost_rows_above, term_h) - emit_rows. Done BEFORE
            // the new-frame emit so the cursor ends in the same
            // place the existing math expects.
            // Cursor's actual physical viewport row at entry: wire_rows - 1.
            // Decisions below must derive from this, NOT from term_h - 1
            // (which assumes the cursor is at the viewport bottom and is
            // wrong whenever the live frame was shorter than the viewport).
            const int wire_rows  = std::min(std::max(1, state.ghost_rows_above_),
                                            term_h);
            const int cursor_row = wire_rows - 1;
            const int emit_rows  = std::min(content_rows, term_h);
            const int start_row  = std::max(0, content_rows - term_h);

            // Where the new frame's top must land for its bottom to coincide
            // with the cursor (cursor_row). If negative, the new frame is
            // taller than the cursor's offset from viewport top — the excess
            // must scroll the viewport so host content above goes into native
            // scrollback (the same effect normal streaming produces).
            const int new_top   = cursor_row - (emit_rows - 1);
            const int extra_top = (new_top > 0) ? new_top : 0;
            const int scroll_n  = (new_top < 0) ? -new_top : 0;

            if (scroll_n > 0) {
                // Frame doesn't fit above the cursor. Walk to the bottom
                // edge, emit scroll_n newlines (each \n at row term_h - 1
                // scrolls the viewport), then move up to the new frame's
                // top row and serialize.
                const int down_to_bottom = (term_h - 1) - cursor_row;
                if (down_to_bottom > 0)
                    ansi::write_cursor_down(out, down_to_bottom);
                out += '\r';
                for (int i = 0; i < scroll_n; ++i) out += '\n';
                if (emit_rows - 1 > 0)
                    ansi::write_cursor_up(out, emit_rows - 1);
                out += '\r';
                serialize(canvas, pool, out, content_rows, start_row);
                out += "\x1b[J";
            } else if (extra_top > 0) {
                // Stale rows sit above the new frame's top — EL-erase them.
                // Walk up to the old frame's top, then EL+LF down past the
                // stale rows. After the loop the cursor sits at col 0 of
                // the new frame's top row.
                if (cursor_row > 0)
                    ansi::write_cursor_up(out, cursor_row);
                out += '\r';
                for (int i = 0; i < extra_top; ++i) {
                    out += "\x1b[K";
                    out += '\n';
                }
                serialize(canvas, pool, out, content_rows, start_row);
                out += "\x1b[J";
            } else {
                // New frame fits exactly above the cursor with no stale
                // rows to clean above. Move up to the new frame's top.
                if (emit_rows - 1 > 0)
                    ansi::write_cursor_up(out, emit_rows - 1);
                out += '\r';
                serialize(canvas, pool, out, content_rows, start_row);
                out += "\x1b[J";
            }
            state.ghost_rows_above_ = 0;
        } else {
            // Fresh state (A): inline-mode growth from cursor.
            // Leading \r anchors the first row at col 0 — inherited shell
            // cursor may be mid-line (e.g. host left output without a
            // trailing newline, or a `read -p` prompt). serialize() only
            // emits \r\n BETWEEN rows; the first row would otherwise
            // start at whatever column the host left the cursor at.
            out += '\r';
            serialize(canvas, pool, out, content_rows);
            state.ghost_rows_above_ = 0;
        }
        if (synchronized_output) out += ansi::sync_end;
        // Cache the new cell buffer for next frame's comparison.  This is
        // the one path that legitimately needs a full memcpy because
        // prev_cells is empty.  Overflow check protects against pathological
        // inputs (e.g. content_rows = INT_MAX from a buggy auto_height).
        const std::size_t new_size = safe_cells(content_rows, W);
        if (new_size == (std::size_t)(-1)) {
            // Pathological size — drop the cache; caller falls back to
            // Divergent on the next render.
            state = std::move(state).reset_state();
            return {std::move(out), std::move(state)};
        }
        if (state.prev_cells_.size() < new_size) state.prev_cells_.resize(new_size);
        std::memcpy(state.prev_cells_.data(), cells, new_size * sizeof(uint64_t));
        state.prev_width_ = W;
        state.prev_rows_  = content_rows;
        state.wire_cursor_rows_ = std::min(content_rows, term_h);
        return {std::move(out), std::move(state)};
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
        state = std::move(state).reset_state();
        return {std::move(out), std::move(state)};
    }
    if (state.prev_cells_.size() < new_total_pre)
        state.prev_cells_.resize(new_total_pre);
    // Resize may have moved storage; re-take the pointer. `prev` is
    // const because the diff scan reads from it; we use a separate
    // mutable handle for the shadow writes.
    prev          = state.prev_cells_.data();
    uint64_t* prev_w = state.prev_cells_.data();

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
    if (!state.decawm_off_) {
        out += "\x1b[?7l";
        state.decawm_off_ = true;
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

        // Rows about to be committed to native scrollback by this
        // frame's bottom-edge \r\n's. Once any of these rows scrolls
        // off, the emulator captures the bytes currently on the wire
        // for that row — forever, into its own scrollback that we
        // cannot rewrite. If prev_cells says "matches canvas" but the
        // wire actually shows old content (stale shadow, wide-char
        // boundary mis-snap, layout shift between frames), that
        // stale content commits.
        //
        // Defence: force a full-row re-emit for every row that this
        // frame will scroll off, plus the topmost still-visible row
        // (next frame's growth will scroll it). Previously the
        // heuristic protected only the topmost about-to-commit row
        // (single y == content_rows - term_h); if the frame grew by
        // more than one row in a single frame, the lower
        // about-to-commit rows still leaked.
        //
        // Rows scrolled off THIS frame: [prev_visible_top, new_visible_top).
        // Topmost still-visible row (next frame's risk): y == new_visible_top.
        // Union: [prev_visible_top, new_visible_top].
        const int new_visible_top = std::max(0, content_rows - term_h);
        const int prev_visible_top = std::max(0, prev_rows - term_h);
        const bool will_scroll_off =
            (content_rows >= term_h)
            && (y >= prev_visible_top)
            && (y <= new_visible_top);

        // Find the changed sub-span. For new rows (no prev), this is
        // [first_non_blank, last_non_blank+1). For the will-scroll-off
        // row, force a full-row consideration so any wire-vs-canvas
        // skew is corrected before the row leaves our control.
        const int x_first_diff_raw = will_scroll_off
                                 ? 0
                                 : first_diff_col(cur_row, prev_row, W);
        const int x_first_diff = will_scroll_off
                                 ? 0
                                 : snap_first_diff_left(x_first_diff_raw,
                                                        cur_row, prev_row, W);

        if (x_first_diff < W) {
            const int x_last_diff_raw = will_scroll_off
                                       ? W - 1
                                       : last_diff_col(cur_row, prev_row, W);
            const int x_last_diff    = will_scroll_off
                                       ? W - 1
                                       : snap_last_diff_right(x_last_diff_raw,
                                                              cur_row, prev_row, W);
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
                // cursor now at col x_end_emit — BUT with DECAWM off,
                // an emit that reached col W-1 leaves the cursor AT W-1
                // rather than advancing to col W. The EL branch below
                // accounts for this so the rightmost cell isn't erased.
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
                // DECAWM-off precondition: if the emit filled through
                // col W-1, cursor sits AT W-1 — \x1b[K from there would
                // erase the cell we just painted. Skip EL in that case:
                // there's nothing to erase to the right of the cursor
                // (cursor IS at the right edge). If the row was empty
                // (need_emit=false), cursor is at x_first_diff which is
                // strictly < W by the entry guard, so EL is safe.
                const bool cursor_at_right_edge =
                    need_emit && (x_end_emit >= W);
                if (!cursor_at_right_edge) {
                    out += "\x1b[K";
                }
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
    //
    // We DO NOT emit ansi::reset here — it has to happen AFTER the
    // shrink path below, otherwise the shrink's re-emit of the bottom
    // row would believe the wire is still in `current_style` while
    // ansi::reset has just moved the wire to id 0. The mismatch would
    // cause the re-emit to skip the SGR transition for the first
    // styled run, drawing those cells in default style.

    // ── Shrink: erase rows past the new content_rows ───────────────────
    // The per-row loop left the cursor at row `last_row_to_visit`
    // (= content_rows - 1) at SOME column — specifically the column
    // where the row's last emit landed it (x_end_emit if the loop
    // emitted, x_first_diff if it only did \x1b[K, col 0 if the row
    // was unchanged-in-common and only \r\n was emitted). \x1b[J
    // wipes from THAT column through end of screen, which is what we
    // want for the rows below, but on the bottom row itself it
    // preserves cells [0, cursor_col) and erases [cursor_col, W).
    // That's correct only if `cursor_col` ≥ last_content_col + 1.
    // The middle-slice-only-changed case (need_el false,
    // x_end_emit < x_last_visible + 1) violates that and erases real
    // content from the bottom row.
    //
    // Make the precondition local: re-anchor the cursor to col 0 with
    // \r, re-emit the bottom row's visible content (idempotent — same
    // cells already on the wire), then \x1b[J. The re-emit positions
    // the cursor exactly at last_content_col + 1; \x1b[J then erases
    // a guaranteed-empty tail on the bottom row plus every row below
    // it. Cost: O(W) bytes for the bottom row at most, paid only when
    // the frame actually shrinks (rare).
    //
    // We also need to keep prev_cells consistent with this re-emit.
    // The bottom row was already shadowed in the per-row loop above
    // (rows < prev_rows hit the `is_new_row=false` path; new rows hit
    // the full-row copy). Re-emitting writes the SAME bytes the diff
    // already wrote, so prev_cells is correct without further action.
    if (content_rows < prev_rows) {
        out += '\r';
        const int last_visible = canvas.last_content_col(last_row_to_visit);
        if (last_visible >= 0) {
            emit_cell_run(canvas, pool, last_row_to_visit,
                          0, last_visible + 1,
                          current_style, out);
        }
        // Reset SGR before erase so erased cells inherit no attrs
        // from the last emitted cell.
        if (current_style != 0) {
            out.append(pool.sgr(0));
            current_style = 0;
        }
        // Now position cursor and erase the rows that the new (shorter)
        // frame no longer covers. There are two cases governed by where
        // the emit left the cursor under DECAWM-off semantics:
        //
        //   (a) last_visible < W-1 (or empty row):
        //       Cursor naturally landed at col last_visible+1 < W
        //       (or col 0 from the leading \r if the row was empty).
        //       \x1b[J from there erases the bottom row's tail plus
        //       every row below. Cursor stays at (last_row_to_visit,
        //       last_visible+1) — the next compose's diff math assumes
        //       cursor row == prev_rows - 1, which holds.
        //
        //   (b) last_visible == W-1 (row fully painted):
        //       Cursor is stuck AT col W-1 (DECAWM-off doesn't advance
        //       past the right edge, ECMA-48 §8.3.118). \x1b[J here
        //       would erase the rightmost cell — the last-column
        //       corruption symptom on full-width content (code-block
        //       right borders, full-width rules). Instead walk down
        //       one row with \r\n, \x1b[J from there (which doesn't
        //       touch last_row_to_visit at all), then \x1b[A back to
        //       restore the cursor-row invariant the next compose
        //       depends on. \n at row `last_row_to_visit + 1 = content_rows`
        //       won't scroll: prev_rows > content_rows means at least
        //       row content_rows existed in the prior frame, so it's
        //       inside the viewport (prev_rows ≤ term_h holds because
        //       wire_cursor_rows clamps prev_rows at term_h on every
        //       compose).
        if (last_visible < W - 1) {
            out += "\x1b[J";
        } else {
            out += "\r\n\x1b[J\x1b[A";
        }
    }

    // SGR reset at the tail: drop any residual style state so the
    // next compose starts from a known floor (current_style sentinel
    // becomes UINT16_MAX on entry; the wire is at id 0). Always safe
    // — the shrink path above already reset to 0 when needed.
    out += ansi::reset;   // drop residual SGR

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
    state.prev_width_ = W;
    state.prev_rows_  = content_rows;
    state.wire_cursor_rows_ = std::min(content_rows, term_h);

    // ── Production shadow-of-wire hash ─────────────────────────────────
    //
    // Hash the entire prev_cells[0, content_rows*W) range with a fast
    // FNV-1a-style fold. verify_shadow() recomputes from the
    // same range before the next compose; mismatch ⇒ someone mutated
    // prev_cells (or the underlying allocation) outside the compose
    // path and the diff would silently emit wrong bytes. The runtime
    // demotes to Divergent on mismatch.
    //
    // Range is the full prev frame [0, content_rows) rather than just
    // the visible viewport: simpler invariant (one hash covers every
    // byte the diff might consult next frame), and commit_prefix
    // handles scrollback shifts by re-hashing after the memmove.
    {
        const uint64_t* shadow_base = state.prev_cells_.data();
        uint64_t h = 14695981039346656037ULL;
        const std::size_t n =
            static_cast<std::size_t>(content_rows) * W;
        for (std::size_t i = 0; i < n; ++i) {
            h ^= shadow_base[i];
            h *= 1099511628211ULL;
        }
        state.shadow_hash_ = h;
    }

    // ── Shadow-of-wire invariant check (debug builds only) ─────────────
    //
    // The single most load-bearing invariant in the inline renderer:
    // `prev_cells[y][x]` must, at the end of every compose, equal
    // `canvas.cells()[y*W + x]` for every (x, y) the wire is about
    // to show — i.e. rows in [updatable_start, content_rows).
    //
    // Every historical "corrupted rows" bug in this file's git log
    // was a version of THIS invariant being temporarily violated:
    // over-commit_scrollback shift, cells_max_y heuristic stale,
    // streaming-markdown cache aliasing, empty-CodeBlock ghost,
    // partial-write residue inflation — all of them showed up
    // downstream as either a `simd::bulk_eq` returning "unchanged"
    // for a row that did change (frame freezes) or returning
    // "changed" for a row that didn't (cursor walks redundantly,
    // emitting bytes that drift the wire away from the shadow).
    //
    // Compile-time gate: define MAYA_DEBUG_SHADOW_VERIFY to enable
    // (off by default; dev/CI builds only). When on, this loop walks
    // every viewport-visible cell and aborts on the first mismatch
    // with a precise (y, x) trace, turning a class of "intermittent
    // ghost rows in production" into immediate test failures.
    //
    // Performance: O(prev_on_screen × W) u64 compares per frame, fully
    // vectorisable. ~1μs on a 200×80 viewport. Never compiled into
    // release.
#ifdef MAYA_DEBUG_SHADOW_VERIFY
    {
        const uint64_t* shadow = state.prev_cells_.data();
        const int verify_start = std::max(0, state.prev_rows_ - prev_on_screen);
        for (int y = verify_start; y < content_rows; ++y) {
            for (int x = 0; x < W; ++x) {
                const uint64_t s = shadow[static_cast<std::size_t>(y) * W + x];
                const uint64_t c = cells [static_cast<std::size_t>(y) * W + x];
                if (s != c) {
                    std::fprintf(stderr,
                        "\n[maya] SHADOW-OF-WIRE INVARIANT VIOLATED "
                        "at (y=%d, x=%d): shadow=0x%016llx canvas=0x%016llx "
                        "first_changed=%d content_rows=%d prev_rows_in=%d W=%d\n",
                        y, x,
                        static_cast<unsigned long long>(s),
                        static_cast<unsigned long long>(c),
                        first_changed, content_rows, prev_rows, W);
                    std::abort();
                }
            }
        }
    }
#endif
    return {std::move(out), std::move(state)};
}

} // namespace maya
