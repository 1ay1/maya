// src/device/render_fullscreen.cpp — the alt-screen path: cell diff.
#include "internal.hpp"

namespace maya::detail {

auto Device::render_fullscreen(const Element& root, int w) -> Status {
    FILE* const prof_out = frame_prof_out();
    const bool prof = prof_out != nullptr;
    const auto t_frame_start = std::chrono::steady_clock::now();
    auto since = [](auto t0) { return ms_since(t0); };
    // ── Fullscreen path: dispatch on coherence variant ──────────────────
    // The front canvas lives only inside FullscreenSynced.  The diff
    // pipeline can only be invoked from inside that lambda — the
    // serialize path is the sole option for Divergent.  std::visit
    // statically rejects any future state we forget to handle.
    const int h = size_.height.raw();
    if (h <= 0) return ok();

    if (canvas_.width() != w || canvas_.height() != h) {
        canvas_.set_style_pool(&pool_);
        canvas_.resize(w, h);
        // The front (if any) was sized for the old terminal — drop it.
        if (std::holds_alternative<coherent::FullscreenSynced>(fs_coherence_))
            fs_coherence_ = coherent::Divergent{};
    }

    // ── Backpressure via non-blocking writer (parity with inline) ──────
    // Output fd is O_NONBLOCK. If the previous frame couldn't be fully
    // drained, its tail sits in the writer's residue. Drain it before
    // composing a new frame; if the wire still won't accept, defer this
    // render entirely — DO NOT compose, because front-canvas diffing
    // would update `front` to reflect bytes the wire hasn't received,
    // breaking the diff invariant. `has_pending_writes()` keeps the
    // outer event loop spinning at ~8 ms intervals until residue clears
    // (see app.hpp:1058 and 1266). Before this change the fullscreen
    // path used `write_raw` and propagated WouldBlock as a hard error,
    // tearing down the program on the first frame that exceeded the
    // tty buffer (common: doom-fire style heavy first frames in a
    // small pty).
    if (writer_->has_residue()) {
        auto d = writer_->try_drain_residue();
        if (!d) {
            if (d.error().kind == ErrorKind::WouldBlock) {
                return ok();   // wire still backed up; retry next tick
            }
            // Hard I/O error — toss residue and demote to Divergent so
            // the next successful render does a full serialize.
            writer_->discard_residue();
            fs_coherence_ = coherent::Divergent{};
            return d;
        }
    }

    // Style ids are uint16_t and pool-local. Truecolor animation can intern
    // thousands of previously unseen fg/bg pairs per frame; retaining them
    // forever eventually exhausts the id space, after which intern() safely
    // falls back to style 0 (plain white). A resize increases the number of
    // sampled pixels and commonly makes the failure appear at that moment.
    //
    // Reserve enough ids for a worst-case next frame (one unique style per
    // visible cell plus modest paint-overdraw). Rebase before saturation,
    // invalidate every cache carrying old ids, and force a coherent full
    // repaint. Ordinary widget apps never approach this branch.
    constexpr std::size_t kUsableStyleIds = 65534;
    const auto visible_cells =
        static_cast<std::size_t>(std::max(1, canvas_.width())) *
        static_cast<std::size_t>(std::max(1, canvas_.height()));
    const auto frame_headroom = std::min(kUsableStyleIds,
        std::max<std::size_t>(4096, visible_cells + 1024));
    if (pool_.overflowed() ||
        pool_.size() >= kUsableStyleIds - frame_headroom) {
        pool_.clear();
        render_detail::clear_component_cache();
        fs_coherence_ = coherent::Divergent{};
        canvas_.clear();
    }

    // Same obligation as the inline and grid paths: a theme swap rewrites
    // what every style ID renders as without changing any ID, so the
    // fullscreen diff against s.front would emit nothing. Go Divergent to
    // force a full re-state, and clear the latch — leaving it set would keep
    // has_deferred_frame() true and spin the loop.
    if (pending_retheme_) {
        pending_retheme_ = false;
        render_detail::clear_component_cache();
        fs_coherence_ = coherent::Divergent{};
        canvas_.clear();
    }

    Status write_status = ok();
    fs_coherence_ = std::visit(overload{
        // Synced → Synced (success) or Synced → Divergent (write fail).
        [&](coherent::FullscreenSynced& s) -> coherent::FullscreenState {
            out_.clear();
            auto opened = RenderPipeline<stage::Idle>::start(canvas_, pool_, theme_, out_)
                .clear()
                .paint(root, layout_nodes_)
                .open_frame(emit_sync_wrapper_);
            std::move(opened).write_diff(s.front).close_frame(emit_sync_wrapper_);
            // write_or_buffer: ships what fits, stashes the rest in
            // residue. The residue is drained at the top of the next
            // render (above); until then `has_residue()` keeps the
            // event loop polling at 8 ms. Hard I/O errors still demote
            // to Divergent — discard_residue() because the buffered
            // tail is from a frame we're about to abandon.
            if (auto wr = writer_->write_or_buffer(out_); !wr) {
                writer_->discard_residue();
                write_status = wr;
                return coherent::Divergent{};       // drop the stale front
            }
            // Front ↔ back swap.  The just-composed canvas content is
            // now the canonical front (the bytes are either on the
            // wire or safely in residue, which drains before next
            // compose); the old front becomes the recyclable back
            // buffer for the next paint.
            std::swap(s.front, canvas_);
            canvas_.reset_damage();
            return coherent::FullscreenSynced{std::move(s.front)};
        },

        // Divergent → Synced (success) or Divergent → Divergent (write fail).
        [&](coherent::Divergent) -> coherent::FullscreenState {
            out_.clear();
            auto opened = RenderPipeline<stage::Idle>::start(canvas_, pool_, theme_, out_)
                .clear()
                .paint(root, layout_nodes_)
                .open_frame(emit_sync_wrapper_);
            // Home + serialize every row (no \x1b[2J — flashes inside DEC
            // sync). serialize() appends \x1b[K per row to wipe stale
            // trailing content.
            out_ += "\x1b[H";
            serialize(canvas_, pool_, out_);
            std::move(opened).close_frame(emit_sync_wrapper_);
            if (auto wr = writer_->write_or_buffer(out_); !wr) {
                writer_->discard_residue();
                write_status = wr;
                return coherent::Divergent{};
            }
            // canvas_ now mirrors what's on the terminal (or will, once
            // residue drains) — promote it to the new front. Allocate
            // a fresh back of matching size for the next paint cycle.
            Canvas new_back(canvas_.width(), canvas_.height(), &pool_);
            Canvas new_front = std::exchange(canvas_, std::move(new_back));
            canvas_.reset_damage();
            return coherent::FullscreenSynced{std::move(new_front)};
        },
    }, fs_coherence_);

    // MAYA_FRAME_PROF for fullscreen frames, in the inline line's format
    // (the fields fullscreen has: total, nodes, w/h, bytes, and the build /
    // layout / paint phases), so tests/frame_phases.py can measure every
    // Program example. Fullscreen frames used to be invisible to it: most
    // of maya's examples run fullscreen, and couldn't be profiled at all.
    if (prof) {
        static std::uint64_t prev_b = 0, prev_l = 0, prev_p = 0;
        const std::uint64_t now_b = render_detail::rt_build_ns();
        const std::uint64_t now_l = render_detail::rt_layout_ns();
        const std::uint64_t now_p = render_detail::rt_paint_ns();
        std::fprintf(prof_out,
            "maya-frame: rt=%.2f cf=%.2f total=%.2f nodes=%zu rows=%d w=%d "
            "bytes=%zu fullscreen ph[b=%.2f l=%.2f p=%.2f]\n",
            since(t_frame_start), 0.0, since(t_frame_start),
            layout_nodes_.size(), canvas_.height(), canvas_.width(), out_.size(),
            (now_b - prev_b) / 1e6, (now_l - prev_l) / 1e6, (now_p - prev_p) / 1e6);
        prev_b = now_b; prev_l = now_l; prev_p = now_p;
        std::fflush(prof_out);
    }
    return write_status;
}

} // namespace maya::detail
