// maya::strata — implementation. See strata.hpp for the design rationale.
//
// The per-frame pipeline:
//
//   1. RECONCILE the active layer against the host's live node suffix
//      (nodes after the sealed frontier), reusing cached Elements whose
//      hash is unchanged and building only the misses. Auto-detect a
//      wholesale swap / rewind (frontier fingerprint + band-anchor key) and
//      self-arm a hard reset.
//   2. DEPOSIT BOOKKEEPING: drop whole front nodes that have fully deposited
//      into native scrollback (the committed_rows_ watermark covers them);
//      mirror handle_resize for width/height changes and rewinds.
//   3. COMPOSE a WINDOWED canvas — band rows [committed_rows_, band_height),
//      each stratum at (its y) - committed_rows_, the straddling node clipped
//      to its visible tail — so the renderer never sees an already-deposited
//      row. MEASURE uses maya's own layout engine at term_cols.
//   4. DRIVE the InlineFrame state machine to emit a minimal diff over the
//      window.
//   5. ADVANCE committed_rows_ by the rows this frame scrolled off the top
//      edge (= win_h - term_h) and shrink the renderer's shadow to match
//      with a single commit(), so a deposited row is never addressed again.

#include "maya/render/strata.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <variant>
#if defined(MAYA_DEBUG_STRATA_TRIPWIRE)
#include <cstdio>
#include <cstdlib>
#endif

#include "maya/render/renderer.hpp"     // render_tree_at, render_detail::build_layout_tree
#include "maya/style/theme.hpp"
#include "maya/terminal/writer.hpp"

namespace maya::strata {

int Strata::measure(const Element& e, int cols) {
    if (cols < 1) return 0;
    measure_nodes_.clear();
    const std::size_t root =
        render_detail::build_layout_tree(e, measure_nodes_, theme::dark);
    if (root >= measure_nodes_.size()) return 0;
    layout::compute(measure_nodes_, root, cols);
    const int h = measure_nodes_[root].computed.size.height.raw();
    return h > 0 ? h : 0;
}

void Strata::reset() {
    band_.clear();
    committed_rows_    = 0;
    prev_total_nodes_  = 0;
    sealed_count_      = 0;
    sealed_rows_total_ = 0;
    sealed_front_fp_   = 0;
    active_rows_       = 0;
    prev_width_        = 0;
    have_prev_         = false;
    coh_ = inline_frame::InlineFrame<inline_frame::Empty>{};
}

void Strata::reset_hard() {
    // Drop the active layer + frontier like reset(), but DON'T reseed the
    // coherence to Empty (which would append from the cursor with no
    // wipe). Instead demote the live coherence to HardReset so the next
    // frame emits \x1b[2J\x1b[3J\x1b[H and repaints from a cleared
    // screen — erasing the old surface on screen and in native scrollback.
    band_.clear();
    committed_rows_    = 0;
    prev_total_nodes_  = 0;
    sealed_count_      = 0;
    sealed_rows_total_ = 0;
    sealed_front_fp_   = 0;
    active_rows_       = 0;
    prev_width_        = 0;
    have_prev_         = false;
    coh_ = std::visit(
        [](auto&& arm) -> inline_frame::InlineCoherence {
            using T = std::decay_t<decltype(arm)>;
            using namespace inline_frame;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm).demote_to_hard_reset();
            else if constexpr (std::is_same_v<T, InlineFrame<Stale>>)
                return std::move(arm).escalate_to_hard_reset();
            else
                // Empty / Fresh / HardReset / Sealed: nothing prior on the
                // wire we must wipe (or already armed). Empty→Fresh appends
                // cleanly; a swap from a never-rendered surface needs no wipe.
                return std::move(arm);
        },
        std::move(coh_));
}

void Strata::demote_soft() {
    // Soft repaint in place (case B) — NO scrollback wipe. Only meaningful
    // from Synced; other states already repaint cleanly next frame.
    if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(&coh_))
        coh_ = std::move(*s).demote_to_stale();
}

FrameStats Strata::frame(std::span<const NodeRef> nodes,
                         const BuildFn& build,
                         int term_cols,
                         TermRows term_rows,
                         StylePool& pool,
                         Writer& writer) {
    using namespace inline_frame;

    FrameStats st;
    st.total_nodes = static_cast<int>(nodes.size());

    const int  term_h        = term_rows.value();
    const bool width_changed  = have_prev_ && (prev_width_  != term_cols);
    const bool height_changed = have_prev_ && (prev_height_ != term_h);
    const bool nodes_shrank   = have_prev_ && (nodes.size() < prev_total_nodes_);
    const int  budget        = std::max(term_h * keep_viewports_, std::max(term_h, 1));

    // A host that truncated its list below our frontier (thread swap into
    // SHORTER content, transcript rewind) invalidates the seal frontier AND
    // strands the old overflow rows in native scrollback. Arm a hard reset
    // so the wipe clears them — the host issues no escape-level reset.
    if (nodes.size() < sealed_count_) {
        reset_hard();
    }

    // AUTONOMOUS WHOLESALE-SWAP DETECTION.
    // The sealed prefix [0, sealed_count_) is append-only by contract: the
    // host enumerates a growing transcript, so the node at the frontier
    // (the last one we sealed) must keep the same key+hash forever. When
    // the host swaps its entire surface — loading a different thread, ^N
    // into a fresh one — it hands a node list where that frontier slot now
    // holds DIFFERENT content. We detect that here by re-folding the
    // frontier node's fingerprint and comparing to the one we stored when
    // we sealed it; a mismatch means the old on-screen transcript AND the
    // rows it pushed to native scrollback must be wiped before the new
    // surface paints. Arm a hard reset ourselves so the host issues NO
    // escape-level reset_inline — it just swaps its model and we notice.
    if (sealed_count_ > 0 && sealed_count_ <= nodes.size()) {
        const NodeRef& front = nodes[sealed_count_ - 1];
        const std::uint64_t fp =
            (front.key * 1099511628211ULL) ^ (front.hash + 0x9e3779b97f4a7c15ULL);
        if (sealed_front_fp_ != 0 && fp != sealed_front_fp_) {
            reset_hard();   // clears band_/frontier + arms the wipe
        }
    }

    // BAND-ANCHOR SWAP DETECTION (independent of sealing).
    // Windowing keeps most content live in band_ rather than sealing it,
    // so sealed_count_ can stay 0 for a long session and the frontier-
    // fingerprint check above never engages. The band itself is the
    // anchor: by the append-only contract, the FRONT live node's key must
    // still appear in the host's node list every frame. When the host
    // swaps its whole surface (thread switch / new thread) or rewinds
    // below the band, that key vanishes from the incoming list — the old
    // on-screen transcript AND its native-scrollback rows must be wiped
    // before the new surface paints. Detect it here and arm a hard reset.
    if (have_prev_ && !band_.empty()) {
        const std::uint64_t anchor = band_.front().key;
        bool present = false;
        for (const NodeRef& nr : nodes) {
            if (nr.key == anchor) { present = true; break; }
        }
        if (!present) reset_hard();
    }

    // ── 1. Reconcile / seed the active layer ─────────────────────────────
    if (band_.empty() && sealed_count_ == 0 && !nodes.empty()) {
        // COLD SEED (first frame, or first after reset()/reset_hard()).
        //
        // Build the bounded keep window from the END of the transcript
        // (newest `budget` rows) and PRE-SEAL the older prefix [0, start)
        // WITHOUT building or emitting it. The pre-seal is the deliberate,
        // documented bounded-resume contract:
        //
        //   • Live-appended session (the common case): those older nodes
        //     were emitted to the wire while they were live, so they are
        //     ALREADY in the terminal's native scrollback. Pre-sealing
        //     records that fact; re-emitting them would duplicate.
        //   • Cold resume of a saved thread: nothing was emitted yet, so
        //     the prefix is NOT on this terminal. Pre-sealing means Strata
        //     renders only the bounded resume window (the newest `budget`
        //     rows) inline; the older history is intentionally NOT
        //     repainted (it would cost O(transcript) and stamp a second
        //     copy of anything the user scrolls back to). This is the
        //     O(viewport) resume guarantee, not content loss.
        //
        // Either way the prefix is treated as immutable scrollback the
        // renderer never addresses. committed_rows_ stays 0: the seeded
        // window is entirely live, and its own overflow (if budget >
        // term_h) deposits normally via Sections 3/5 this very frame.
        std::vector<Stratum> seeded;
        int acc = 0;
        std::size_t start = nodes.size();
        while (start > 0) {
            const NodeRef& nr = nodes[start - 1];
            Element e = build(nr.key);
            int h = measure(e, term_cols);
            if (h < 1) h = 1;
            seeded.push_back(Stratum{nr.key, nr.hash, nr.terminal, std::move(e), h});
            ++st.builds;
            acc += h;
            --start;
            if (acc >= budget) break;
        }
        std::reverse(seeded.begin(), seeded.end());
        band_           = std::move(seeded);
        committed_rows_ = 0;     // the seeded window is wholly live
        sealed_count_   = start; // [0, start) pre-sealed; never built/emitted.
        // Baseline the frontier fingerprint so a later wholesale swap is
        // detectable even against a cold-resumed prefix.
        if (start > 0) {
            const NodeRef& f = nodes[start - 1];
            sealed_front_fp_ =
                (f.key * 1099511628211ULL) ^ (f.hash + 0x9e3779b97f4a7c15ULL);
        }
    } else if (!nodes.empty()) {
        // Steady reconcile of the live suffix nodes[sealed_count_ .. end].
        std::unordered_map<std::uint64_t, std::size_t> idx;
        idx.reserve(band_.size() * 2 + 1);
        for (std::size_t k = 0; k < band_.size(); ++k) idx.emplace(band_[k].key, k);
        std::vector<bool> used(band_.size(), false);

        std::vector<Stratum> nb;
        if (nodes.size() > sealed_count_) nb.reserve(nodes.size() - sealed_count_);
        for (std::size_t i = sealed_count_; i < nodes.size(); ++i) {
            const NodeRef& nr = nodes[i];
            auto it = idx.find(nr.key);
            if (it != idx.end() && !used[it->second]
                && band_[it->second].hash == nr.hash) {
                Stratum s = std::move(band_[it->second]);
                used[it->second] = true;
                s.terminal = nr.terminal;
                if (width_changed) {
                    int h = measure(s.element, term_cols);
                    s.height = h < 1 ? 1 : h;
                }
                nb.push_back(std::move(s));
            } else {
                Element e = build(nr.key);
                int h = measure(e, term_cols);
                if (h < 1) h = 1;
                nb.push_back(Stratum{nr.key, nr.hash, nr.terminal, std::move(e), h});
                ++st.builds;
            }
        }
        band_ = std::move(nb);
    }

    // ── 2. Deposit bookkeeping + resize recovery ───────────────────────
    // Strata keeps `keep_viewports` of content live in band_ for resize
    // re-measure, but it must never re-emit a row the terminal already
    // OWNS (one that scrolled past the top edge). `committed_rows_` is the
    // running count of band rows, from the top, that have physically
    // deposited into native scrollback. Section 3 WINDOWS them out of the
    // composed canvas, so the renderer's diff can only ever touch rows that
    // are still on screen (or scrolling off for the FIRST time this frame).
    //
    // Here we (a) drop whole front nodes that have fully deposited (pure
    // bookkeeping — they are in scrollback, not in the canvas), and
    // (b) mirror handle_resize: a width change wipes + repaints fresh (the
    // old-width scrollback is the terminal's), a height change soft-repaints
    // the new viewport in place. Both reset the watermark: after a wipe the
    // band reseeds; after a height change the window slides to the bottom
    // term_h rows and is repainted, so any prior committed math is moot.
    auto drop_committed_front_nodes = [&] {
        while (band_.size() > 1
               && committed_rows_ >= band_.front().height) {
            const Stratum& f = band_.front();
            sealed_front_fp_ =
                (f.key * 1099511628211ULL) ^ (f.hash + 0x9e3779b97f4a7c15ULL);
            committed_rows_ -= f.height;
            band_.erase(band_.begin());
            ++sealed_count_;
        }
    };

    if (have_prev_ && std::holds_alternative<InlineFrame<Synced>>(coh_)) {
        auto s = std::get<InlineFrame<Synced>>(std::move(coh_));
        if (width_changed) {
            // Old-width scrollback belongs to the terminal; wipe + repaint
            // the re-measured active layer fresh.
            committed_rows_ = 0;
            coh_ = std::move(s).demote_to_hard_reset();
            st.full_repaint = true;
        } else if (height_changed || nodes_shrank) {
            // Slide the window so win_h ≤ term_h and soft-repaint (case B
            // repaints the bottom term_h rows in place, no scrollback wipe).
            // Triggered by a height change OR a rewind (the live node count
            // dropped, so the on-screen rows above the new, shorter band are
            // stale and must be erased by the case-B repaint).
            // committed_rows_ is MONOTONIC: a deposited row can never be
            // un-scrolled, so on a height GROW we keep the old watermark
            // (win_h just shrinks below term_h); on a height SHRINK we raise
            // it so the newly-overflowing rows are excluded and the case-B
            // emit covers the whole window with no stale rows left above.
            int bh = 0;
            for (const auto& s : band_) bh += s.height;
            committed_rows_ = std::max(committed_rows_, bh - term_h);
            if (committed_rows_ < 0) committed_rows_ = 0;
            coh_ = std::move(s).demote_to_stale();
            st.full_repaint = true;
        } else {
            drop_committed_front_nodes();
            coh_ = std::move(s);
        }
    } else {
        drop_committed_front_nodes();
    }
    st.coherence = static_cast<int>(coh_.index());

    // ── 3. Compose the WINDOWED active layer into one canvas ─────────────
    // The window is band rows [committed_rows_, band_height): rows already
    // deposited into native scrollback are excluded so the renderer never
    // re-stamps them. Each node paints at (its global y) - committed_rows_;
    // a node entirely above the window is skipped, and the straddling node
    // is clipped to its still-visible tail by painting it at a negative
    // origin (render_tree_at clips above row 0).
    int band_height = 0;
    for (const auto& s : band_) band_height += s.height;
    if (committed_rows_ > band_height) committed_rows_ = band_height;
    const int win_h = std::max(band_height - committed_rows_, 1);
    if (canvas_.width() != term_cols || canvas_.height() < win_h) {
        canvas_.set_style_pool(&pool);
        canvas_.resize(term_cols, win_h);
    }
    canvas_.clear();
    {
        int y = -committed_rows_;   // global band y, shifted into window space
        for (const auto& s : band_) {
            if (s.height > 0 && y + s.height > 0)
                render_tree_at(s.element, canvas_, pool, theme::dark,
                               0, y, term_cols, s.height);
            y += s.height;
        }
    }

    // ── 4. Render: drive the InlineFrame state machine ───────────────────
    auto lift = [](auto&& outcome) -> InlineCoherence {
        return std::visit([](auto&& a) -> InlineCoherence { return std::move(a); },
                          std::move(outcome));
    };

    coh_ = std::visit(
        [&](auto&& arm) -> InlineCoherence {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Empty>>) {
                auto fresh = std::move(arm).seed();
                return lift(std::move(fresh).render(
                    canvas_, content_rows(canvas_), term_rows, pool, writer, true));
            } else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                return lift(std::move(arm).render(
                    canvas_, content_rows(canvas_), term_rows, pool, writer, true));
            } else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                // Windowing (Section 3) excludes already-deposited rows, so
                // the canvas the diff sees is the still-mutable viewport plus
                // at most this frame's fresh growth — never a row the terminal
                // already owns. The only residual hazard is a shadow/wire
                // desync (verify fails); recover with an in-place soft
                // repaint (case B, no scrollback wipe).
                auto wit = arm.verify();
                if (!wit) {
                    st.full_repaint = true;
                    return std::move(arm).demote_to_stale();
                }
                return lift(std::move(arm).render(
                    canvas_, content_rows(canvas_), term_rows, pool, writer,
                    *std::move(wit), true));
            } else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                return lift(std::move(arm).render(
                    canvas_, content_rows(canvas_), term_rows, pool, writer, true));
            } else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                return lift(std::move(arm).render(
                    canvas_, content_rows(canvas_), term_rows, pool, writer, true));
            } else {
                static_assert(std::is_same_v<T, InlineFrame<Sealed>>);
                return std::move(arm);
            }
        },
        std::move(coh_));

    // ── 5. Advance the deposit watermark ────────────────────────────
    // The render just laid the windowed canvas (win_h rows) into the
    // viewport, scrolling the top max(0, win_h - term_h) rows past the
    // top edge and into native scrollback. Those rows are now immutable.
    // Record them in committed_rows_ so NEXT frame's window excludes them,
    // and shrink the renderer's shadow by the same amount via commit() so
    // its prev_rows stays ≤ term_h — the renderer never holds, addresses,
    // or re-diffs a deposited row again.
    if (auto* sy = std::get_if<InlineFrame<Synced>>(&coh_)) {
        const int now = sy->rows();
        const int deposited = now - term_h;
        if (deposited > 0) {
            auto marker = sy->scrollback_marker(deposited);
            coh_ = std::move(*sy).commit(marker);
            committed_rows_ += deposited;
            sealed_rows_total_ += static_cast<std::uint64_t>(deposited);
            st.sealed_now += deposited;
        }
    }

    // ── Deposition-invariant tripwire (debug builds only) ───────────────
    //
    // The single load-bearing guarantee of the depositional model: AFTER a
    // frame, the renderer's diffable shadow holds at most one viewport of
    // rows. If prev_rows > term_h, a row that already scrolled into native
    // scrollback is STILL addressable by next frame's diff — the exact
    // re-emit-a-deposited-row duplication the windowed compose exists to
    // prevent (test_strata proves it can't happen; this catches a future
    // regression at the source instead of as a ghost row in production).
    //
    // Also asserts committed_rows_ stays a sane prefix of the band: it must
    // be in [0, band_height], or the window math (Section 3) would clip the
    // wrong rows. Both checks are O(1).
    //
    // Compile-time gate: define MAYA_DEBUG_STRATA_TRIPWIRE (off by default;
    // dev/CI builds). Never compiled into release.
#ifdef MAYA_DEBUG_STRATA_TRIPWIRE
    {
        int dbg_band_h = 0;
        for (const auto& s : band_) dbg_band_h += s.height;
        if (const auto* sy = std::get_if<InlineFrame<Synced>>(&coh_)) {
            if (sy->rows() > term_h) {
                std::fprintf(stderr,
                    "\n[maya] STRATA DEPOSITION INVARIANT VIOLATED: "
                    "prev_rows=%d > term_h=%d after deposit "
                    "(committed_rows=%d band_height=%d live_nodes=%zu) — a "
                    "deposited row is still in the diffable window and will "
                    "be re-emitted next frame.\n",
                    sy->rows(), term_h, committed_rows_, dbg_band_h,
                    band_.size());
                std::abort();
            }
        }
        if (committed_rows_ < 0 || committed_rows_ > dbg_band_h) {
            std::fprintf(stderr,
                "\n[maya] STRATA WATERMARK INVARIANT VIOLATED: "
                "committed_rows=%d outside [0, band_height=%d].\n",
                committed_rows_, dbg_band_h);
            std::abort();
        }
    }
#endif

    active_rows_ = 0;
    for (const auto& s : band_) active_rows_ += s.height;
    st.live_nodes = static_cast<int>(band_.size());
    st.live_rows  = active_rows_;
    prev_width_   = term_cols;
    prev_height_  = term_h;
    prev_total_nodes_ = nodes.size();
    have_prev_    = true;
    return st;
}

} // namespace maya::strata
