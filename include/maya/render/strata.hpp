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
// Deposition is ROW-GRANULAR, not whole-turn. Strata keeps a few viewports
// of content live for width re-measure, but composes a WINDOWED canvas that
// excludes rows already deposited into native scrollback (tracked by an
// internal committed-rows watermark). The renderer therefore never sees —
// let alone re-emits — a row the terminal already owns, even when a single
// turn is taller than the viewport. A whole turn is dropped from the active
// layer only once all of its rows have deposited (pure bookkeeping).
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

    // Like reset(), but ARM a hard reset so the NEXT frame wipes the
    // terminal (viewport + native scrollback via \x1b[2J\x1b[3J\x1b[H)
    // before repainting. The sanctioned recovery for a WHOLESALE CONTENT
    // SWAP into different/shorter content (thread switch / new thread):
    // the old transcript on screen AND the rows it committed to native
    // scrollback are both erased, so nothing strands above the new
    // surface. Destructive to host scrollback above the frame — only for
    // an explicit, user-initiated swap.
    void reset_hard();

    // Like reset_hard(), but for a wholesale surface swap where NOTHING
    // was ever deposited into native scrollback (committed_rows_ == 0 and
    // sealed_rows_total_ == 0) — e.g. the empty-thread WELCOME screen,
    // which is clamped to fit the viewport and never scrolls a row off.
    // The old surface lives entirely on screen, so it must be scrubbed,
    // but a \x1b[3J would erase the user's PRE-EXISTING terminal history
    // above the frame (shell output from before agentty launched, or a
    // prior session) — content Strata never owned and must not touch.
    // Drops the band + frontier like reset_hard() but arms an in-place
    // case-B soft repaint (viewport scrub, NO scrollback wipe) instead.
    void reset_swap_soft();

    // Force the next frame to soft-repaint the active layer in place
    // (case-B: walk cursor up, repaint viewport, erase below; NO
    // scrollback wipe). For a host-driven "redraw" (Ctrl-L) that must
    // scrub ghost cells without touching native scrollback. No-op unless
    // the current coherence is Synced.
    void demote_soft();

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

    // Content hash of the top `head_rows` rendered rows of one node at
    // `cols`. The steady-frame reflow guard compares this across frames to
    // tell a TRUE reflow of a deposited row (content changed → hard reset)
    // from a benign whole-node hash bump (animation cadence term, or an
    // append below the fold — the parked rows are byte-identical → no
    // reset). Renders into reflow_canvas_ and folds the packed cells.
    std::uint64_t committed_head_content_hash(
        const Stratum& s, int head_rows, int cols, StylePool& pool);

    std::vector<Stratum> band_;          ///< The active layer, in order.
    // Rows at the TOP of band_ that have physically deposited into native
    // scrollback already and must NOT be re-emitted. The band keeps these
    // rows (for width-resize re-measure) but compose WINDOWS them out: the
    // canvas handed to the renderer starts `committed_rows_` rows down from
    // the band top. This is the row-granular analog of sealing a whole
    // node — it lets a node that only PARTIALLY scrolled off contribute
    // its still-visible tail to the canvas while its committed head is
    // excluded, so the renderer's per-row diff never re-stamps a row the
    // terminal already owns. Reset to 0 on any width change (hard reset
    // wipes scrollback) or wholesale swap.
    int           committed_rows_ = 0;
    std::size_t   prev_total_nodes_ = 0;  ///< Host node count last frame (rewind detect).
    std::size_t   sealed_count_ = 0;     ///< Leading host nodes already set.
    std::uint64_t sealed_rows_total_ = 0;
    // Fingerprint of the last node sealed past the fold (key folded with
    // hash). The sealed prefix [0, sealed_count_) is append-only: a host
    // that swaps its whole surface (thread switch / new thread) hands a
    // node list whose frontier node no longer matches this fingerprint.
    // frame() reads it to AUTO-DETECT the swap and arm a hard reset, so
    // the host never issues an escape-level reset_inline itself. 0 = no
    // node sealed yet (nothing to validate against).
    std::uint64_t sealed_front_fp_ = 0;
    // Identity + content hash of the band node that OWNS the last committed
    // row (the row at committed_rows_ - 1) as of last frame. Used to detect
    // a live (non-terminal) row that already deposited into native
    // scrollback and then REFLOWED: if that node is still live and its hash
    // changed, the deposited copy is now stale and a hard reset is the only
    // duplicate-free recovery (Section 2 steady branch). 0 = nothing
    // committed yet.
    std::uint64_t committed_tail_key_  = 0;
    std::uint64_t committed_tail_hash_ = 0;
    int           active_rows_ = 0;
    int           prev_width_  = 0;
    int           prev_height_ = 0;
    int           keep_viewports_ = 3;
    bool          have_prev_   = false;

    inline_frame::InlineCoherence coh_{
        inline_frame::InlineFrame<inline_frame::Empty>{} };
    Canvas        canvas_;
    // Scratch canvas for committed_head_content_hash — kept separate from
    // canvas_ (the per-frame compose target) so hashing a node's deposited
    // head never disturbs the live compose state.
    Canvas        reflow_canvas_;
    std::vector<layout::LayoutNode> measure_nodes_;  ///< Reused scratch.
};

} // namespace maya::strata
