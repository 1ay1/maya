// maya::strata — implementation. See strata.hpp for the design rationale.
//
// The per-frame pipeline:
//
//   1. RECONCILE the active layer against the host's live node suffix
//      (nodes after the sealed frontier), reusing cached Elements whose
//      hash is unchanged and building only the misses.
//   2. MEASURE every live node at term_cols — maya's own layout engine,
//      the same path render_tree runs — and COMPOSE them into one canvas,
//      each stratum stamped at its running y-offset.
//   3. SEAL whole terminal strata that have scrolled past the fold, but
//      never an in-viewport row: the seal count is read from THIS canvas
//      at THIS width, so it equals what physically scrolled off, exactly.
//   4. DRIVE the InlineFrame state machine to emit a minimal diff, then
//      advance the scrollback frontier with a single commit() and DROP the
//      sealed strata from the active layer.

#include "maya/render/strata.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <variant>

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
    sealed_count_      = 0;
    sealed_rows_total_ = 0;
    active_rows_       = 0;
    prev_width_        = 0;
    have_prev_         = false;
    coh_ = inline_frame::InlineFrame<inline_frame::Empty>{};
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
    const int  budget        = std::max(term_h * keep_viewports_, std::max(term_h, 1));

    // A host that truncated its list below our frontier (thread swap done
    // without reset(), transcript rewind) invalidates the seal frontier.
    // Fall back to a fresh seed.
    if (nodes.size() < sealed_count_) {
        band_.clear();
        sealed_count_ = 0;
    }

    // ── 1. Reconcile / seed the active layer ─────────────────────────────
    if (band_.empty() && sealed_count_ == 0 && !nodes.empty()) {
        // Cold seed: keep ~budget rows from the END, pre-seal the rest
        // WITHOUT building them. On a live-appended session those older
        // nodes were emitted while live (so they are already in native
        // scrollback); on a cold resume this is the bounded-resume window.
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
        band_         = std::move(seeded);
        sealed_count_ = start;   // [0, start) are pre-sealed; never built/emitted.
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

    // ── 2. Seal-first + resize recovery (the single commit point) ───────
    // BEFORE composing, commit the PREVIOUS frame's overflow — the whole
    // terminal strata that have scrolled past the CURRENT viewport — and
    // drop them from the active layer. This is the analog of agent_session
    // committing its frozen prefix: it keeps the rendered canvas ≈ one
    // viewport, so a stale re-anchor (height resize) never walks up into
    // committed scrollback and strands a duplicate of the oldest rows.
    //
    // The commit count is the summed height of whole TERMINAL strata (whose
    // heights are stable once terminal), capped at prev_rows - term_h, so it
    // equals exactly what physically scrolled off — measured by maya, at
    // maya's width. Then mirror handle_resize: width change → hard reset
    // (old-width scrollback is the terminal's; wipe + repaint fresh), height
    // change → soft repaint in place (case B, no scrollback wipe).
    auto seal_front = [&](inline_frame::InlineFrame<inline_frame::Synced>& s) {
        const int off = std::max(0, s.rows() - term_h);
        int cum = 0; std::size_t n = 0;
        for (const auto& st0 : band_) {
            if (!st0.terminal) break;            // never seal a growing node
            if (n + 1 >= band_.size()) break;    // always keep ≥ one live node
            if (cum + st0.height > off) break;   // never commit an in-viewport row
            cum += st0.height; ++n;
        }
        if (n == 0 || cum == 0) return;
        auto marker = s.scrollback_marker(cum);
        s = std::move(s).commit(marker);
        band_.erase(band_.begin(), band_.begin() + static_cast<std::ptrdiff_t>(n));
        sealed_count_      += n;
        sealed_rows_total_ += static_cast<std::uint64_t>(cum);
        st.sealed_now       = cum;
    };

    if (have_prev_ && std::holds_alternative<InlineFrame<Synced>>(coh_)) {
        auto s = std::get<InlineFrame<Synced>>(std::move(coh_));
        if (width_changed) {
            // arm.rows() is an old-width count; don't seal against it. The
            // wipe repaints the (re-measured) active layer fresh.
            coh_ = std::move(s).demote_to_hard_reset();
            st.full_repaint = true;
        } else {
            seal_front(s);
            if (height_changed) {
                coh_ = std::move(s).demote_to_stale();
                st.full_repaint = true;
            } else {
                coh_ = std::move(s);
            }
        }
    }
    st.coherence = static_cast<int>(coh_.index());

    // ── 3. Compose the (reduced) active layer into one canvas ────────────
    int band_height = 0;
    for (const auto& s : band_) band_height += s.height;
    const int alloc_h = std::max(band_height, 1);
    if (canvas_.width() != term_cols || canvas_.height() < alloc_h) {
        canvas_.set_style_pool(&pool);
        canvas_.resize(term_cols, alloc_h);
    }
    canvas_.clear();
    {
        int y = 0;
        for (const auto& s : band_) {
            if (s.height > 0)
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
                // Safety net: a live stratum still in the band changed and
                // shifted a committed prefix row. Seal-first keeps the canvas
                // ≈ viewport so this rarely fires, but keep the proven
                // commit-overflow + soft-repaint recovery (no scrollback wipe).
                const int prev_rows = arm.rows();
                const int new_rows  = content_height(canvas_);
                if (prev_rows > term_h && new_rows < prev_rows) {
                    const int overflow = prev_rows - term_h;
                    if (!arm.scrollback_prefix_matches(canvas_, overflow)) {
                        auto marker    = arm.scrollback_marker(overflow);
                        auto committed = std::move(arm).commit(marker);
                        st.full_repaint = true;
                        return std::move(committed).demote_to_stale();
                    }
                }
                auto wit = arm.verify();
                if (!wit) {
                    const int pr = arm.rows();
                    if (pr > term_h) {
                        const int overflow = pr - term_h;
                        auto marker    = arm.scrollback_marker(overflow);
                        auto committed = std::move(arm).commit(marker);
                        st.full_repaint = true;
                        return std::move(committed).demote_to_stale();
                    }
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

    active_rows_ = 0;
    for (const auto& s : band_) active_rows_ += s.height;
    st.live_nodes = static_cast<int>(band_.size());
    st.live_rows  = active_rows_;
    prev_width_   = term_cols;
    prev_height_  = term_h;
    have_prev_    = true;
    return st;
}

} // namespace maya::strata
