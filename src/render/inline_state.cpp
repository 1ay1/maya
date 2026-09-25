// src/render/inline_state.cpp — the inline frame's state: terminal rows,
// finalize/commit, the scrollback-prefix checks, content height.
#include "serialize_internal.hpp"

namespace maya {

using namespace serialize_detail;

TermRows query_term_rows(platform::NativeHandle handle) noexcept {
    const auto sz = platform::query_terminal_size(handle);
    const int h = sz.height.raw();
    return TermRows{h > 0 ? h : 24};
}



// ShadowWitness producer. The shadow hash is maintained incrementally
// (per-row hashes in state.row_hashes_, XOR-combined into
// shadow_hash_). verify re-folds the row-hash array — O(prev_rows) of
// u64 XORs, not O(prev_rows x width) cell reads — and checks it against
// the cached combine. A mismatch means prev_cells (or row_hashes_) was
// mutated outside the compose path: the shadow is poisoned, return
// nullopt and let the runtime demote to Divergent.
//
// Note we still return a populated witness on the "fresh" path so the
// witness carries a concrete value, never the UINT64_MAX sentinel.
std::optional<ShadowWitness> verify_shadow(const InlineFrameState& state) noexcept {
    // Fresh state: no prior shadow exists, so trivially "matches".
    // Witness carries the current empty-state hash.
    if (state.shadow_hash_ == static_cast<uint64_t>(-1) ||
        state.prev_width_ <= 0 || state.prev_rows_ <= 0) {
        return ShadowWitness{&state, 0ULL};
    }

    // Row-hash array must be in lockstep with prev_rows_; if it isn't,
    // something cleared it out-of-band — treat as poisoned.
    if (state.row_hashes_.size() != static_cast<std::size_t>(state.prev_rows_))
        return std::nullopt;

    // shadow_hash_ is maintained INCREMENTALLY at the end of every
    // compose (XOR the changed rows out/in — see the incremental-combine
    // block in compose_inline_frame_impl), so it already equals the fold
    // of row_hashes_ by construction on the hot path. Re-folding the
    // whole array here to "double-check" is O(prev_rows) per frame and
    // was the residual linear `cf`-growth cost on a tall transcript: at
    // 7000 committed rows it XORed 7000 u64s every single frame purely
    // to reconfirm a value compose already maintained exactly.
    //
    // The re-fold's only real job is to catch an OUT-OF-BAND mutation of
    // row_hashes_ / prev_cells_ (a code path that wrote the shadow
    // without going through compose). No such path exists, and the
    // lockstep-size check above already rejects the one realistic
    // corruption (an out-of-band clear/resize). In debug builds we still
    // pay the full re-fold as a tripwire; release trusts the maintained
    // hash and returns O(1).
#ifndef NDEBUG
    const uint64_t h = combine_rows(state.row_hashes_);
    if (h != state.shadow_hash_) return std::nullopt;
    return ShadowWitness{&state, h};
#else
    return ShadowWitness{&state, state.shadow_hash_};
#endif
}

FinalizeResult InlineFrameState::finalize() && noexcept {
    std::string out;
    // Hardware-caret epilogue may have left the physical cursor ABOVE
    // the resting row (at the caret cell). Return it to the frame's
    // bottom-left first so the host shell resumes below the frame, not
    // mid-box. Row-relative (CUD from a known offset) — same discipline
    // as the epilogue itself.
    if (cursor_row_offset_ > 0) {
        ansi::write_cursor_down(out, cursor_row_offset_);
        out += '\r';
    }
    if (cursor_hidden_) out.append("\x1b[?25h");
    if (decawm_off_)    out.append("\x1b[?7h");
    // Hardware-caret cosmetics are GLOBAL terminal state — restore them
    // for the next program iff this session touched them. `0 q` =
    // terminal-default shape; OSC 112 = reset cursor color.
    if (cursor_shape_ != 0) out.append("\x1b[0 q");
    if (cursor_color_ != 0) out.append("\x1b]112\x1b\\");
    InlineFrameState s{std::move(*this)};
    s.cursor_hidden_ = false;
    s.decawm_off_ = false;
    s.cursor_row_offset_ = 0;
    s.cursor_shown_ = false;
    s.cursor_shape_ = 0;
    s.cursor_color_ = 0;
    return FinalizeResult{std::move(out), std::move(s)};
}

InlineFrameState InlineFrameState::committed(ScrollbackMarker marker) && noexcept {
    InlineFrameState s{std::move(*this)};
    const int rows = marker.rows();

    // ── Generation guard (type-theoretic single-source-of-truth) ──────
    // The marker captured the issuing state's generation. If it no
    // longer matches THIS state's gen_, the marker was measured against
    // a superseded frame (an intervening compose / commit / recovery
    // advanced the state) — committing its row count now would be the
    // deposit-watermark second-accountant bug: dropping rows the CURRENT
    // shadow never actually pushed to scrollback. Reject it. In a debug
    // build this is LOUD (the same discipline as the scrollback-invariant
    // gate abort in Runtime::render); in release it is a safe no-op —
    // never a silent MISAPPLY. An empty marker (gen_==0, rows==0) falls
    // through the rows<=0 no-op below regardless.
    if (!marker.empty() && marker.generation() != s.gen_) {
#ifndef NDEBUG
        // Opt-in tripwire (MAYA_GATE_ABORT=1): same policy as the
        // scrollback-invariant gate in Runtime::render — the reject-the-
        // stale-marker fallthrough below is a safe no-op, so a daily-driven
        // Debug build should self-heal rather than die; CI bug hunts can
        // export MAYA_GATE_ABORT=1 to make it LOUD and fatal.
        if (std::getenv("MAYA_GATE_ABORT")) {
            std::fprintf(stderr,
                "[maya] FATAL: ScrollbackMarker generation mismatch "
                "(marker.gen=%llu state.gen=%llu rows=%d) — commit issued "
                "from a superseded accountant. Unset MAYA_GATE_ABORT to "
                "no-op instead.\n",
                static_cast<unsigned long long>(marker.generation()),
                static_cast<unsigned long long>(s.gen_), rows);
            std::fflush(stderr);
            std::abort();
        }
#endif
        return s;   // release: reject the stale marker, no mutation
    }

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

    // Shift the per-row hash array to match the cell memmove. Rows
    // [rows, old_prev_rows) move to [0, new_prev_rows). Their cell
    // CONTENT is unchanged by the shift, but each row's hash mixes in
    // its index (hash_row), so a row that moved from y+rows to y must
    // be re-hashed at its new position. Re-derive from the shifted
    // cells; this is O(new_prev_rows) which is bounded post-trim.
    if (s.shadow_hash_ != static_cast<uint64_t>(-1)
        && s.row_hashes_.size() == static_cast<std::size_t>(s.prev_rows_ + rows)) {
        s.row_hashes_.resize(static_cast<std::size_t>(s.prev_rows_));
        const uint64_t* cbase = s.prev_cells_.data();
        for (int y = 0; y < s.prev_rows_; ++y)
            s.row_hashes_[static_cast<std::size_t>(y)] =
                hash_row(cbase + static_cast<std::size_t>(y) * W, W, y);
        s.shadow_hash_ = combine_rows(s.row_hashes_);
    } else if (s.shadow_hash_ != static_cast<uint64_t>(-1)) {
        // Row-hash array out of lockstep (external reset) — fall back
        // to a full recompute so the next verify stays consistent.
        s.row_hashes_.assign(static_cast<std::size_t>(s.prev_rows_), 0);
        const uint64_t* cbase = s.prev_cells_.data();
        for (int y = 0; y < s.prev_rows_; ++y)
            s.row_hashes_[static_cast<std::size_t>(y)] =
                hash_row(cbase + static_cast<std::size_t>(y) * W, W, y);
        s.shadow_hash_ = combine_rows(s.row_hashes_);
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
    ++s.gen_;   // content-advancing: invalidates any outstanding marker
    return s;
}

bool InlineFrameState::scrollback_prefix_matches(
    const Canvas& canvas, int rows) const noexcept
{
    if (rows <= 0) return true;
    // Width must match or the row stride differs and a cell compare is
    // meaningless — treat as "does not match" so the caller recovers.
    const int W = prev_width_;
    if (W <= 0 || canvas.width() != W) return false;
    // Can't compare more rows than either buffer holds.
    if (rows > prev_rows_) return false;
    const std::size_t need = static_cast<std::size_t>(rows) * W;
    if (prev_cells_.size() < need) return false;
    if (canvas.cell_count() < need) return false;
    const uint64_t* a = prev_cells_.data();
    const uint64_t* b = canvas.cells();
    const bool match = std::memcmp(a, b, need * sizeof(uint64_t)) == 0;
    if (!match && std::getenv("MAYA_DEBUG_GATE")) {
        // Diagnostic (env-gated): find the first mismatching row, then
        // dump a ±8-row window from BOTH buffers so re-wraps / shifts
        // are visible in context.
        int first_bad = -1;
        std::string idxs;
        for (int y = 0; y < rows; ++y) {
            if (std::memcmp(a + (std::size_t)y * W, b + (std::size_t)y * W,
                            (std::size_t)W * sizeof(uint64_t)) != 0) {
                if (first_bad < 0) first_bad = y;
                idxs += std::to_string(y);
                idxs += ' ';
            }
        }
        auto dump_row = [&](const uint64_t* base, int y) {
            std::string s;
            for (int x = 0; x < W; ++x) {
                Cell c = Cell::unpack(base[(std::size_t)y * W + x]);
                char32_t ch = c.character;
                s += (ch >= 32 && ch < 127) ? (char)ch : '?';
            }
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };
        // Style-only mismatches (identical glyphs, different style ids)
        // are invisible in the char dump — print the per-cell style-id
        // pairs for each mismatching column so lifecycle restyles
        // (dim flips, color swaps) are attributable.
        auto dump_styles = [&](int y) {
            std::string s;
            int shown = 0;
            for (int x = 0; x < W && shown < 16; ++x) {
                const uint64_t pa = a[(std::size_t)y * W + x];
                const uint64_t pb = b[(std::size_t)y * W + x];
                if (pa == pb) continue;
                Cell ca = Cell::unpack(pa), cb = Cell::unpack(pb);
                char ga = (ca.character >= 32 && ca.character < 127)
                              ? (char)ca.character : '?';
                char gb = (cb.character >= 32 && cb.character < 127)
                              ? (char)cb.character : '?';
                s += " x" + std::to_string(x) + ":'";
                s += ga; s += "'s"; s += std::to_string(ca.style_id);
                s += "->'"; s += gb; s += "'s";
                s += std::to_string(cb.style_id);
                ++shown;
            }
            return s;
        };
        std::fprintf(stderr,
            "[gate] prefix mismatch (prev_rows=%d rows=%d W=%d)\n"
            "[gate] all mismatch rows: %s\n",
            prev_rows_, rows, W, idxs.c_str());
        if (first_bad >= 0) {
            const int lo = first_bad > 8 ? first_bad - 8 : 0;
            const int hi = std::min(rows, first_bad + 10);
            for (int y = lo; y < hi; ++y) {
                const bool bad = std::memcmp(
                    a + (std::size_t)y * W, b + (std::size_t)y * W,
                    (std::size_t)W * sizeof(uint64_t)) != 0;
                std::fprintf(stderr, "  %c prev[%3d]: '%s'\n",
                             bad ? '*' : ' ', y, dump_row(a, y).c_str());
                std::fprintf(stderr, "  %c new [%3d]: '%s'\n",
                             bad ? '*' : ' ', y, dump_row(b, y).c_str());
                if (bad)
                    std::fprintf(stderr, "    cells[%3d]:%s\n",
                                 y, dump_styles(y).c_str());
            }
        }
    }
    return match;
}

// Structure-only prefix compare: glyphs/hyperlink/width, ignoring style_id.
//
// Deliberately NOT a memcmp — masking has to touch each cell, so it is a word
// loop. Affordable because it only runs on frames where the full compare
// already FAILED: the rare something-changed-above-the-fold case, never in
// steady streaming.
bool InlineFrameState::scrollback_prefix_structure_matches(
    const Canvas& canvas, int rows) const noexcept
{
    if (rows <= 0) return true;
    const int W = prev_width_;
    if (W <= 0 || canvas.width() != W) return false;
    if (rows > prev_rows_) return false;
    const std::size_t need = static_cast<std::size_t>(rows) * W;
    if (prev_cells_.size() < need) return false;
    if (canvas.cell_count() < need) return false;

    const uint64_t* a = prev_cells_.data();
    const uint64_t* b = canvas.cells();
    for (std::size_t i = 0; i < need; ++i) {
        if (((a[i] ^ b[i]) & cell_structure_mask) != 0) return false;
    }
    return true;
}

bool InlineFrameState::scrollback_prefix_window_matches(
    const Canvas& canvas, int lo, int hi) const noexcept
{
    if (lo < 0) lo = 0;
    if (hi <= lo) return true;
    const int W = prev_width_;
    if (W <= 0 || canvas.width() != W) return false;
    // Can't compare more rows than either buffer holds.
    if (hi > prev_rows_) return false;
    const std::size_t hi_need = static_cast<std::size_t>(hi) * W;
    if (prev_cells_.size() < hi_need) return false;
    if (canvas.cell_count() < hi_need) return false;
    const std::size_t off = static_cast<std::size_t>(lo) * W;
    const std::size_t span = static_cast<std::size_t>(hi - lo) * W;
    return std::memcmp(prev_cells_.data() + off,
                       canvas.cells() + off,
                       span * sizeof(uint64_t)) == 0;
}

int content_height(const Canvas& canvas) noexcept {
    // O(1): canvas tracks max_y_ during painting. -1 ⇒ nothing was
    // ever written this frame ⇒ zero rows of content.
    //
    // Returning 0 (not 1) is load-bearing for the inline path: the
    // run-loop's empty-frame guard (`if (ch <= 0) ...` in src/device/render_inline.cpp
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

std::optional<ScrollbackProof> check_scrollback(
    const InlineFrameState& state, const Canvas& canvas, int term_h) noexcept
{
    const int prev_rows = state.prev_rows_;
    // Frame fits the viewport: nothing overflowed, no committed prefix
    // exists, the obligation is vacuously discharged. Issue a vacuous
    // proof (state_ nullptr, overflow_ 0) so render's signature stays
    // uniform without the caller distinguishing the two cases.
    if (prev_rows <= term_h) {
        return ScrollbackProof{nullptr, 0, /*valid=*/true};
    }
    // Overflowed: the top `overflow` rows have scrolled off and are
    // immutable in native scrollback. The diff is scrollback-safe iff
    // the shadow's committed prefix is byte-identical to what the canvas
    // claims scrolled off.
    //
    // BOUNDED GATE. A full memcmp of the whole [0, overflow) prefix is
    // O(overflow x width) and is the dominant per-frame cost on a long
    // streaming turn: a 14k-row transcript re-memcmp'd ~13 MB EVERY
    // frame purely to reconfirm a prefix that only ever grows at its
    // BOTTOM edge. The corruption this gate exists to catch — a
    // committed-prefix SHIFT (a card above the viewport shrank/grew,
    // sliding every row below it, including the fold-adjacent rows,
    // upward) — is a BLOCK move: if ANY committed row shifts, the rows
    // immediately above the fold (the newest committed rows, the ones a
    // shift pushes across the term_h boundary) shift too. So comparing a
    // bounded WINDOW just above the fold detects every shift that could
    // strand a duplicate, at O(1) cost.
    //
    // The window must be at least as tall as the largest single-frame
    // committed-prefix growth, so no newly-committed row escapes the
    // compare between frames, AND tall enough that a shift originating
    // some rows ABOVE the fold still reaches into the window. kGateWindow
    // (256 rows) is empirically the value at which the PTY oracles report
    // ZERO gate recoveries across every shape (a 64-row window let one
    // oracle shape's settle-shift escape the window and trip a
    // full-compare recovery); it still dwarfs any real per-frame growth
    // (a fat batched SSE delta is a few dozen rows). The clean-grow gate
    // re-runs the FULL compare on any frame that grew by MORE than the
    // window in one step (the grow<=kGateWindow guard below), so a burst
    // can never smuggle an unverified row across the fold.
    //
    // FULL-COMPARE FALLBACK. The bounded window is only sound for a
    // clean append-GROW from the previously-verified state. Any frame
    // that is NOT a monotone grow of the same-width prefix — a shrink
    // (content got shorter: a fold collapsed, a card settled), a width
    // change, or the FIRST overflow (no prior verified prefix) — gets
    // the full memcmp. Those are exactly the frames where a prefix row
    // ABOVE the window could change without touching the window, and
    // they are rare (once per settle/fold, never in steady streaming).
    const int overflow = prev_rows - term_h;
    constexpr int kGateWindow = 256;
    // content_rows for THIS frame = the canvas's painted height. A
    // monotone grow means the new frame is at least as tall as the
    // shadow (nothing above the fold could have moved UP).
    const int content_rows = content_height(canvas);
    const int grow = content_rows - prev_rows;
    const bool clean_grow =
        grow >= 0                                 // monotone grow (or equal)
        && grow <= kGateWindow                    // no burst past the window
        && canvas.width() == state.prev_width_;   // same-width prefix
    if (clean_grow && overflow > kGateWindow) {
        // Verify only the window just above the fold. Any shift reaches
        // it (block move), so a match here proves the whole prefix is
        // stable for THIS append-grow frame.
        const int win_lo = overflow - kGateWindow;
        if (!state.scrollback_prefix_window_matches(canvas, win_lo, overflow)) {
            // Window mismatch — a shift reached the fold. Confirm with a
            // full compare before recovering (the window could, in
            // principle, false-positive on a within-window-only change
            // that is actually benign; the full compare is the source of
            // truth). Cheap relative to the recovery it may trigger.
            if (!state.scrollback_prefix_matches(canvas, overflow)
                && !state.scrollback_prefix_structure_matches(canvas, overflow))
                return std::nullopt;
        }
        return ScrollbackProof{&state, overflow, /*valid=*/true};
    }
    if (!state.scrollback_prefix_matches(canvas, overflow)) {
        // ── A RESTYLE IS NOT A SHIFT ──────────────────────────────────
        //
        // The full compare failed, but this gate's job is narrow: catch a
        // committed-prefix SHIFT, where a row above the viewport MOVED and
        // maya's idea of native scrollback is therefore wrong.
        //
        // A retheme fails the full compare without moving anything — swapping
        // schemes re-interns every style, so `style_id` changes in nearly
        // every cell while every glyph stays put. Treating that as corruption
        // is what made the theme browser recover (full-viewport repaint, ~31
        // KB) on every arrow key on any thread long enough to overflow the
        // viewport: measured as a Synced->Stale demote on 35 of 35 keypresses.
        //
        // So re-check ignoring style_id. Identical structure means the prefix
        // did not move and the scrollback is still valid.
        //
        // NOTE for whoever reads this next: passing the gate is necessary but
        // NOT sufficient for a retheme to look right. The diff that follows
        // also compares packed cells, and a re-interned style_id can collide
        // with an old one — so the caller must ALSO force a repaint when a
        // swap happened, which is why Runtime's Synced arm checks
        // retheme_repaint_ BEFORE calling this.
        if (!state.scrollback_prefix_structure_matches(canvas, overflow))
            return std::nullopt;
    }
    return ScrollbackProof{&state, overflow, /*valid=*/true};
}

} // namespace maya
