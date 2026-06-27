#pragma once
// maya::strata — the depositional inline renderer
//
// ─────────────────────────────────────────────────────────────────────────
// THE PROBLEM IT SOLVES
//
// An inline app (a REPL, a chat transcript, an agent session) grows
// without bound but the terminal can only redraw the bottom `term_rows`
// rows — everything above the fold has scrolled into the terminal's own
// native scrollback and is PHYSICALLY IMMUTABLE. The classic failure is
// that the application keeps the whole transcript "live" and, on a cache
// miss / resize / re-measure, RE-EMITS a turn whose top rows already
// scrolled off — stamping a second copy below the fold. The turn doubles
// in scrollback. Every prior fix in agentty tried to make a host-side
// commit count EXACTLY match what maya physically scrolled off; that is a
// knife-edge because two parties (host + renderer) measure height
// independently and drift under resize.
//
// THE DESIGN
//
// Strata removes the second party. Content is deposited like sediment:
//
//   • The recent turns are a thin, MUTABLE *active layer* — the only
//     thing Strata ever lays out, diffs, or emits. Bounded to a few
//     viewports, so per-frame cost is O(viewport) no matter how long the
//     session runs.
//
//   • The instant a turn scrolls past the fold AND is terminal (done
//     growing), it *sets*: Strata emits it one last time, advances a
//     monotonic scrollback frontier (the high-water mark), and DROPS it
//     from the active layer. It is now an immutable *stratum* owned by
//     the terminal. Strata never measures, builds, or addresses it again.
//
//   • A cache miss is therefore never a correctness problem. A miss can
//     only ever touch a node still IN the active layer — i.e. still
//     on-screen and rewritable. Sealed strata are not in the tree to be
//     missed. Worst case a miss rebuilds one visible turn; it can never
//     re-emit a sealed row.
//
// Because the seal count is measured from Strata's OWN canvas at Strata's
// OWN width — the same render that just went to the wire — it is exactly
// the number of rows that scrolled off, by construction. There is no host
// height, no commit accounting, no width to drift against. The host hands
// Strata a flat list of {key, hash, terminal} node refs and a lazy
// builder; Strata does everything else.
//
// This is NOT a virtual DOM: there is no reconciliation of a whole
// mutable tree against another. It is a one-way depositional pipeline —
// content settles, hardens, and is forgotten by maya but preserved by the
// terminal.
// ─────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "maya/element/element.hpp"
#include "maya/layout/yoga.hpp"
#include "maya/render/canvas.hpp"
#include "maya/render/inline_frame.hpp"
#include "maya/render/serialize.hpp"   // ContentRows, TermRows, content_rows

namespace maya {
class Writer;
class StylePool;
}

namespace maya::strata {

// ─────────────────────────────────────────────────────────────────────────
// NodeRef — the host's per-frame, per-node description. Cheap POD.
// ─────────────────────────────────────────────────────────────────────────
//
// The host passes the FULL ordered node list every frame (an append-only
// transcript). These are two integers and a bool; Strata builds an actual
// Element only on a miss, via the BuildFn below.
struct NodeRef {
    std::uint64_t key  = 0;   ///< Stable identity across frames.
    std::uint64_t hash = 0;   ///< Changes iff the node's rendered output changes.
    bool terminal      = false; ///< True once the node is done growing → may seal.
};

// Builds the Element for one node. Invoked ONLY on a miss (new node, or a
// node whose hash changed). Never called for a sealed node.
using BuildFn = std::function<Element(std::uint64_t key)>;

// ─────────────────────────────────────────────────────────────────────────
// FrameStats — what happened this frame (for tests, profiling, mouse anchor).
// ─────────────────────────────────────────────────────────────────────────
struct FrameStats {
    int total_nodes = 0;   ///< Nodes the host handed us.
    int live_nodes  = 0;   ///< Un-sealed nodes in the active layer after seal.
    int live_rows   = 0;   ///< Content rows in the active layer after seal.
    int sealed_now  = 0;   ///< Rows sealed into native scrollback THIS frame.
    int builds      = 0;   ///< build() calls this frame (= misses).
    int coherence   = 0;   ///< inline_frame variant index that ran (2 == Synced).
    bool full_repaint = false; ///< A wipe/soft-repaint happened (resize/recover).
};

// ─────────────────────────────────────────────────────────────────────────
// Strata — the renderer. One per inline surface; survives across frames.
// ─────────────────────────────────────────────────────────────────────────
class Strata {
public:
    Strata() = default;
    Strata(const Strata&)            = delete;
    Strata& operator=(const Strata&) = delete;
    Strata(Strata&&)                 = default;
    Strata& operator=(Strata&&)      = default;

    // Render one frame.
    //
    //   nodes      — the host's FULL ordered node list (append-only).
    //   build      — lazy Element builder, called only on a miss.
    //   term_cols  — terminal width; the single source of truth for layout.
    //   term_rows  — terminal height witness (query_term_rows in production,
    //                term_rows_for_test in tests).
    //   pool       — style pool (shared with the host's StylePool).
    //   writer     — the wire.
    //
    // The host issues NO commit_scrollback and keeps NO heights. Strata
    // reconciles the active layer, measures every live node at term_cols,
    // seals whole nodes that have scrolled past the fold, advances the
    // scrollback frontier itself, and emits a minimal diff over the bounded
    // active layer.
    FrameStats frame(std::span<const NodeRef> nodes,
                     const BuildFn& build,
                     int term_cols,
                     TermRows term_rows,
                     StylePool& pool,
                     Writer& writer);

    // Drop the active layer and forget the frontier — for a wholesale
    // surface swap (e.g. loading a different thread). The next frame seeds
    // fresh. Does NOT wipe the terminal; the caller decides that.
    void reset();

    // How many leading host nodes have set into scrollback (monotonic
    // across a session, reset() returns it to 0).
    [[nodiscard]] std::size_t sealed_nodes() const noexcept { return sealed_count_; }
    // Total rows deposited into native scrollback over the session.
    [[nodiscard]] std::uint64_t sealed_rows() const noexcept { return sealed_rows_total_; }
    // Content rows currently in the active layer.
    [[nodiscard]] int active_rows() const noexcept { return active_rows_; }

    // Keep this many viewports of recent content live (resize headroom +
    // re-wrappable scrollback). Lower = leaner canvas; higher = more
    // context survives a width change. Floored at one viewport.
    void set_keep_viewports(int v) noexcept { keep_viewports_ = v < 1 ? 1 : v; }

private:
    struct Stratum {
        std::uint64_t key  = 0;
        std::uint64_t hash = 0;
        bool          terminal = false;
        Element       element;     ///< Cached build; reused while hash holds.
        int           height = 0;  ///< Rows at the current width (maya's measure).
    };

    // Layout-engine measure of one Element at `cols` — the same
    // build_layout_tree + compute path render_tree runs, so it equals what
    // the renderer will paint. One-sided: returns 0 only on a degenerate
    // width/tree (caller floors to 1).
    int measure(const Element& e, int cols);

    std::vector<Stratum> band_;          ///< The active layer, in order.
    std::size_t   sealed_count_ = 0;     ///< Leading host nodes already set.
    std::uint64_t sealed_rows_total_ = 0;
    int           active_rows_ = 0;
    int           prev_width_  = 0;
    int           prev_height_ = 0;
    int           keep_viewports_ = 3;
    bool          have_prev_   = false;

    inline_frame::InlineCoherence coh_{
        inline_frame::InlineFrame<inline_frame::Empty>{} };
    Canvas        canvas_;
    std::vector<layout::LayoutNode> measure_nodes_;  ///< Reused scratch.
};

} // namespace maya::strata
