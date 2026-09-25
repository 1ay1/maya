// src/device/render.cpp — one frame: width check, theme edge, then a path.
#include "internal.hpp"

namespace maya::detail {

auto Device::render(const Element& root) -> Status {
    io_log("render");

    const int prev_known_w = size_.width.raw();
    if (prev_known_w <= 0) return ok();

    // Per-frame width reconciliation. `term_h` is re-queried every frame
    // (below) so a missed/coalesced SIGWINCH can't desync the viewport
    // height — but WIDTH only updated in handle_resize(). If a resize
    // event is dropped (kitty/tmux coalescing, a fast drag, a size that
    // differed at launch), the renderer keeps composing at the stale
    // width while the terminal is a different size: every row mis-wraps
    // and the diff's \r / cursor-up math lands on the wrong physical
    // rows — the intermittent flicker / corruption symptom. Treat a
    // per-frame width delta exactly like a resize: update size_, bump
    // the generation, and route the next compose through the
    // width-changed reset path (HardReset inline / Divergent fullscreen)
    // so it repaints clean at the true width. Single TIOCGWINSZ, same
    // ioctl that already backs query_term_rows.
    {
        const auto live = platform::query_terminal_size(output_handle_);
        const int live_w = live.width.raw();
        if (live_w > 0 && live_w != prev_known_w) {
            // Hysteresis: a genuine resize persists; a transient TIOCGWINSZ
            // glitch (kitty's alternating 1-2 col flap) does not. Require the
            // same off-width on two consecutive frames before acting, so a
            // single-frame flap never triggers a resize/repaint storm.
            if (live_w == width_candidate_) {
                handle_resize();
                width_candidate_ = 0;
            } else {
                width_candidate_ = live_w;   // first sighting — wait for confirm
            }
        } else {
            width_candidate_ = 0;            // width matches; clear any candidate
        }
    }
    const int w = size_.width.raw();
    if (w <= 0) return ok();

    // Grid backend: emit a binary cell frame instead of ANSI. Self-contained
    // (own snapshot + diff), never touches the ANSI witness machine below.
    if (grid_mode_) {
        note_theme_swap();
        return render_grid_frame(root);
    }

    render_ctx_.width       = w;
    render_ctx_.height      = size_.height.raw();
    render_ctx_.auto_height = is_inline();
    RenderContextGuard ctx_guard(render_ctx_);

    // Consume the theme edge BEFORE anything can return early.
    //
    // StylePool::retheme() is an edge detector: it compares theme::live()
    // against the theme it last built its SGR cache for, and STORES the new
    // one on the way out. So the edge is consumed by whoever calls it first,
    // and it can only ever be observed once.
    //
    // This used to be called further down, past the inline path's coalesce
    // gate. On a congested wire that gate returns early, so retheme() was
    // not reached and the edge stayed pending — which is fine for ONE
    // deferred frame, because the re-fire picks it up. It is not fine across
    // TWO keypresses: publish B, coalesce, publish C, then re-fire. The
    // stored theme is still A, so retheme() compares C-vs-A, reports one
    // swap, and B is never painted at all. Holding Down in the theme browser
    // on a long thread is exactly that interleaving: big frames leave writer
    // residue, residue raises the congestion EWMA, and the gate starts
    // firing — which is why it works on a short thread and starts skipping
    // every other entry once the transcript grows.
    //
    // Hoisting it here makes the observation unconditional: whatever else
    // this call does or does not do, the swap is seen on the frame the theme
    // actually moved, and the repaint flags it raises are sticky (cleared
    // only after a frame composes), so the deferred compose still performs
    // the work.
    note_theme_swap();

    return is_inline() ? render_inline(root, w) : render_fullscreen(root, w);
}

} // namespace maya::detail
