// src/device/render_grid.cpp — the Grid backend and the off-wire warmup.
#include "internal.hpp"

namespace maya::detail {

// ============================================================================
// Device::warmup_render — pre-populate cross-frame component cache
// ============================================================================
//
// Hot path on resume of a heavy thread: the FIRST render() pays full
// layout + paint over the rehydrated frozen tree (tens to hundreds of
// ms for tool-heavy threads). The cells are then captured into the
// renderer's hash-keyed ComponentCache and every subsequent frame is
// a memcpy-blit per cached entry (sub-millisecond).
//
// warmup_render() is the same render_tree() call, into a private
// canvas, NOT touching writer_ / coherence state. The cache state it
// leaves behind is what makes the next real render() take the fast
// path.
//
// Width/height: matches the live canvas_'s width (cached cells are
// width-keyed and a width mismatch invalidates the entry); height
// gets `auto_height=true` and grows under content.
auto Device::render_grid_frame(const Element& root) -> Status {
    const int w = size_.width.raw();
    if (w <= 0) return ok();

    render_ctx_.width       = w;
    render_ctx_.height      = size_.height.raw();
    render_ctx_.auto_height = true;
    RenderContextGuard ctx_guard(render_ctx_);

    // A theme swap re-derives every interned style's SGR bytes but leaves
    // style IDS untouched, so the grid diff — which compares packed
    // (glyph, style_id) cells against grid_prev_cells_ — sees every cell as
    // unchanged and emits nothing. Force a full re-state so the new colours
    // reach the wire. Same reason the ANSI path raises retheme_repaint_.
    if (pending_retheme_) {
        pending_retheme_ = false;
        grid_need_full_  = true;
        render_detail::clear_component_cache();
    }

    // Paint the tree — same path as the ANSI inline render, so all the layout
    // + component-cache machinery is reused.  We keep it simple vs render():
    // no bounded-clear preservation (the grid diff below IS the bounded
    // update), just a full clear + paint + one grow-and-retry.
    constexpr int kMinCanvasHeight = 500;
    if (canvas_.width() != w || canvas_.height() < kMinCanvasHeight) {
        const bool width_changed = (canvas_.width() != w);
        canvas_.set_style_pool(&pool_);
        canvas_.resize(w, std::max(kMinCanvasHeight, canvas_.height()));
        grid_need_full_ = true;   // reallocation invalidates the snapshot
        // On a WIDTH change (host window resized) the committed scrollback was
        // laid out at the old width and is no longer valid to align against.
        // A terminal reflows: drop the committed prefix so the whole viewport
        // re-renders at the new width from row 0.  (Height-only growth of the
        // canvas doesn't invalidate committed rows.)
        if (width_changed) grid_committed_rows_ = 0;
    }
    canvas_.reset_clips();
    canvas_.clear();
    render_tree(root, canvas_, pool_, theme_, layout_nodes_, /*auto_height=*/true);
    int ch = content_height(canvas_);
    if (!layout_nodes_.empty()) {
        const int needed = layout_nodes_[0].computed.size.height.raw();
        if (needed > canvas_.height()) {
            const int headroom = std::max(64, needed / 4);
            canvas_.resize(w, needed + headroom);
            canvas_.clear();
            render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                        /*auto_height=*/true);
            ch = content_height(canvas_);
            grid_need_full_ = true;
        }
    }
    const int content_h = std::max(0, ch);

    // ── TERMINAL SCROLLBACK MODEL ──
    // grid_committed_rows_ is the count of canvas rows PERMANENTLY pushed to the
    // host's scrollback (written once, never re-emitted).  The live viewport is
    // canvas rows [grid_committed_rows_, content_h), shown at term_h tall.  When
    // the un-committed content exceeds term_h, the OVERFLOW at the top scrolls
    // into history: we emit those exact rows as a Commit frame (the host
    // appends them to scrollback) and advance grid_committed_rows_.  Because the
    // viewport origin only moves forward by exactly what we committed, and
    // committed rows are never re-emitted, nothing can duplicate.
    int term_h = size_.height.raw();
    if (term_h <= 0) term_h = content_h;

    // ── TERMINAL SCROLL ──
    // A terminal's screen is term_h rows; when content grows past it the top
    // rows SCROLL OFF into scrollback.  grid_committed_rows_ counts rows that
    // have already scrolled off: they are emitted exactly ONCE (as a Commit
    // frame carrying their glyphs) and then never rendered again, so the host
    // can keep them as history (its buffer above the home marker) with no
    // duplication.  The screen is the un-committed tail [committed, content_h).
    //
    // Reset the committed prefix if the transcript shrank below it (thread
    // switch / reset / reflow) — the old history no longer aligns.
    if (grid_committed_rows_ > content_h) grid_committed_rows_ = 0;

    // A terminal NEVER moves rows to scrollback on its own: only an explicit
    // scroll from the app (LF at the bottom row, or SU) does that, just as it
    // only clears cells when the app sends ED/EL.  agentty never scrolls in
    // grid mode -- it redraws a screen -- so we must not invent scrolls here.
    // Committing on "content taller than the screen" froze live rows (the
    // composer/status) into history while agentty kept re-rendering them: the
    // duplicated composer + status bar.  The screen is simply the bottom
    // term_h rows of the content, redrawn in place; clearing is handled by the
    // full re-state below whenever the content height moves.
    grid_committed_rows_ = 0;

    // ── EXPLICIT COMMITS (host-side scrollback freeze) ──
    // The app's frozen-block ledger commits N front rows via
    // the commit_scrollback effect -> commit_inline_prefix -> grid_pending_commit_.
    // Those rows' TEXT is already present and correct in the host buffer (we
    // emitted it on earlier frames), so a commit needs NO glyphs on the wire:
    // a header-only Commit(n) tells the host "freeze your top n live lines
    // into immutable history".  The content tree is n rows shorter this
    // frame; shifting our diff snapshot up by n aligns prev row (y+n) with
    // new row y, so the subsequent per-row diff sees the survivors as
    // UNCHANGED and emits nothing for them — the commit costs O(1) on the
    // wire and zero repaint in the host.  Without the shift (the old code's
    // full re-state), every commit forced thousands of host row-patches: the
    // flicker and the destroyed scroll anchors.
    const int pending_commit = std::exchange(grid_pending_commit_, 0);
    out_.clear();
    if (pending_commit > 0) {
        render::emit_commit(pending_commit, out_);
        const int prev_rows_now = grid_prev_rows_;
        const int keep = std::max(0, prev_rows_now - pending_commit);
        if (keep > 0 && grid_prev_w_ == w &&
            static_cast<std::size_t>(prev_rows_now) * w == grid_prev_cells_.size()) {
            // Shift the snapshot up: prev row (y + committed) becomes row y.
            std::memmove(grid_prev_cells_.data(),
                         grid_prev_cells_.data() +
                             static_cast<std::size_t>(pending_commit) * w,
                         static_cast<std::size_t>(keep) * w * sizeof(std::uint64_t));
            grid_prev_cells_.resize(static_cast<std::size_t>(keep) * w);
            grid_prev_rows_ = keep;
            // The prev content height shrank by the same amount: keep the
            // growth heuristic below from misreading the shift as a resize.
            if (grid_prev_content_h_ > 0)
                grid_prev_content_h_ =
                    std::max(0, grid_prev_content_h_ - pending_commit);
        } else {
            // Snapshot can't be aligned (width changed mid-commit or empty):
            // fall back to a full re-state of the (shorter) live surface.
            grid_need_full_ = true;
        }
    }

    // ── INLINE: EMIT THE WHOLE CONTENT ──
    // agentty runs INLINE: it deliberately renders more than one screen and
    // relies on the terminal keeping the overflow as scrollback.  Clamping the
    // emitted rows to the bottom term_h threw that history away — the host had
    // exactly one screen, so there was nothing to scroll and no scrollbar.
    // Emit every content row; the host buffer becomes the scrollback and its
    // window shows the tail, which is precisely what a terminal does.
    const int view_top = 0;
    const int rows = std::max(1, content_h);

    // Content-height changes: the old code forced a FULL re-state of every
    // row on ANY height change — agentty streams a line, content grows by 1,
    // and the entire multi-thousand-row transcript was re-emitted and
    // re-patched into the host buffer (the slowness AND the flicker).  In
    // maya's inline layout, GROWTH appends rows at the bottom: existing rows
    // keep their index, so the per-row diff below handles growth exactly —
    // unchanged prefix emits nothing, new tail rows differ from the
    // (zero-padded) snapshot and are emitted.  Only a SHRINK still forces a
    // full re-state: rows shifted or vanished and the host must truncate via
    // Resize + re-stated survivors (stale rows must never linger).
    if (content_h < grid_prev_content_h_) grid_need_full_ = true;
    grid_prev_content_h_ = content_h;

    // Snapshot the visible rows as packed cells for the diff.  y is a VIEWPORT
    // row [0,view_h); the canvas row it reads is view_top+y (the bottom window
    // of the content).  grid and ANSI never share mutable diff state.
    auto snapshot_row = [&](int y, std::uint64_t* dst) {
        for (int x = 0; x < w; ++x) dst[x] = canvas_.get_packed(x, view_top + y);
    };

    // out_ was cleared above (before Commit emission — the Commit frame must
    // survive into this frame's write).

    // Width change or SHRINK re-states the surface; pure GROWTH takes the
    // diff branch (existing rows keep their index; new tail rows are emitted
    // as changed).  Including growth in dims_changed would re-state the whole
    // transcript on every streamed line — the bug this rewrite removes.
    const bool dims_changed = (grid_prev_w_ != w || rows < grid_prev_rows_);
    if (dims_changed || grid_need_full_) {
        render::emit_resize(w, rows, out_);
        render::GridCursor cur{rows > 0 ? rows - 1 : 0, 0, false};
        // FULL frame: re-state every visible row.  emit_diff reads canvas rows
        // [view_top, view_top+view_h); passing base_row = -view_top makes the
        // emitted row numbers viewport-relative [0, view_h).
        std::vector<int> all;
        all.reserve(static_cast<std::size_t>(rows));
        for (int y = 0; y < rows; ++y) all.push_back(view_top + y);
        render::emit_diff(canvas_, pool_, all, /*base_row=*/-view_top, &cur, out_);
        grid_need_full_ = false;
    } else {
        // GROWTH: tell the host the new surface height first (cheap — its
        // resize only appends blank lines on grow; truncation happens solely
        // in the full-re-state path).
        if (rows > grid_prev_rows_)
            render::emit_resize(w, rows, out_);
        // DIFF: row-compare vs the snapshot, emit only changed rows.  Rows
        // beyond the snapshot (fresh growth) have no prev to compare — they
        // are new content, always emitted.  For each changed row we also note
        // the FIRST differing column (`col_lo`): the Diff frame is an overlay
        // keyed by (row,col) on the host, so we only need to emit that row's
        // changed suffix.  During the streaming glide a row's leading columns
        // are stable and only the tail advances, so this cuts per-frame wire
        // bytes sharply while staying byte-identical on screen.
        std::vector<int> changed;
        std::vector<int> changed_cols;
        for (int y = 0; y < rows; ++y) {
            if (y >= grid_prev_rows_) {
                changed.push_back(view_top + y);   // fresh row: whole row is new
                changed_cols.push_back(0);
                continue;
            }
            const std::uint64_t* prev =
                &grid_prev_cells_[static_cast<std::size_t>(y) * w];
            int first_diff = -1;
            for (int x = 0; x < w; ++x)
                if (prev[x] != canvas_.get_packed(x, view_top + y)) { first_diff = x; break; }
            if (first_diff >= 0) {
                changed.push_back(view_top + y);   // canvas row
                changed_cols.push_back(first_diff);
            }
        }
        if (!changed.empty()) {
            render::GridCursor cur{rows > 0 ? rows - 1 : 0, 0, false};
            render::emit_diff(canvas_, pool_, changed, /*base_row=*/-view_top,
                              &cur, out_, &changed_cols);
        }
    }

    // Growth without full re-state reaches here with rows > grid_prev_rows_:
    // the diff loop above compared the shared prefix; the appended tail rows
    // were compared against zero (empty snapshot) and emitted iff non-empty.
    // Update the snapshot to the just-emitted frame.
    grid_prev_cells_.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(w), 0);
    for (int y = 0; y < rows; ++y)
        snapshot_row(y, &grid_prev_cells_[static_cast<std::size_t>(y) * w]);
    grid_prev_w_    = w;
    grid_prev_rows_ = rows;

    if (out_.empty()) return ok();
    if (auto wr = writer_->write_or_buffer(out_); !wr) {
        writer_->discard_residue();
        grid_need_full_ = true;   // next frame re-states everything
        return wr;
    }
    return ok();
}

// Warm the cross-frame component cache by laying out + painting `root` into a
// scratch canvas (populates the thread_local cache), WITHOUT touching the wire.
// Same paint path as render()'s inline branch (so cache entries match: keys are
// width-keyed and a width mismatch invalidates the entry); height
// gets `auto_height=true` and grows under content.
void Device::warmup_render(const Element& root) {
    const int w = canvas_.width();

    // Scratch canvas — same pool as the live render so captured style
    // ids stay valid when blit'd. Seed height at the current live
    // canvas height so we usually avoid the grow-and-retry below.
    const int seed_h = std::max(64, canvas_.height());
    Canvas scratch(w, seed_h, &pool_);
    scratch.clear();

    std::vector<layout::LayoutNode> nodes;
    nodes.reserve(layout_nodes_.size() + 64);
    render_tree(root, scratch, pool_, theme_, nodes, /*auto_height=*/true);

    // If content overflowed the seed height, grow once and re-render
    // — mirrors the live render() path so the cache entries that get
    // captured are at the same width/height as the next live frame.
    // Same layout-height regrow gate as the live render() path (see the
    // "hidden chrome" rationale there): content_height under-reports when
    // the boundary rows are blank, so key on the layout's needed height.
    if (!nodes.empty()) {
        int needed = nodes[0].computed.size.height.raw();
        if (needed > scratch.height()) {
            scratch.resize(w, needed + 8);
            scratch.clear();
            render_tree(root, scratch, pool_, theme_, nodes,
                        /*auto_height=*/true);
        }
    }
    // scratch is dropped here; the side effect we wanted — the
    // thread_local component_cache holding cells keyed by every
    // hash_id under `root` — persists to the next render() call.
}

} // namespace maya::detail
