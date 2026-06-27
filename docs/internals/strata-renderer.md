# Strata — the depositional inline renderer

`maya::strata::Strata` (`include/maya/render/strata.hpp`,
`src/render/strata.cpp`) is a self-contained inline renderer that makes
**cache misses and resizes structurally incapable of corrupting native
scrollback**, with **O(viewport) per-frame cost no matter how long the
session runs** — and it asks the host for *nothing* beyond a flat list of
node identities.

## The problem

An inline app (a REPL, a chat transcript, an agent session) grows without
bound, but a terminal can only redraw the bottom `term_rows` rows.
Everything above the fold has scrolled into the terminal's own native
scrollback and is **physically immutable**. The classic failure: the app
keeps the whole transcript "live" and, on a cache miss / re-measure /
resize, **re-emits a turn whose top rows already scrolled off** — stamping
a second copy below the fold. The turn doubles in scrollback.

Every host-side attempt to fix this tries to make a commit count *exactly*
equal what the renderer physically scrolled off. That is a knife-edge,
because **two parties measure height independently** (the host's row
accounting and the renderer's layout) and they drift under resize — most
visibly on a phone over SSH, where the soft keyboard fires a resize storm
and the freeze-time terminal width disagrees with the renderer's live one.

## The idea: deposition, not reconciliation

Strata removes the second party. Content is deposited like sediment:

- The recent turns are a thin, **mutable active layer** — the only thing
  Strata ever lays out, diffs, or emits. Bounded to ≈ one viewport, so
  per-frame work is O(viewport).
- The instant a turn scrolls past the fold **and** is terminal (done
  growing), it **sets**: Strata commits it to native scrollback, advances a
  monotonic frontier (a high-water mark that only rises), and **drops it
  from the active layer**. It is now an immutable **stratum** owned by the
  terminal. Strata never measures, builds, or addresses it again.

This is **not** a virtual DOM — there is no reconciliation of a whole
mutable tree against another. It is a one-way pipeline: content settles,
hardens, and is forgotten by maya but preserved by the terminal.

### Why a cache miss can never corrupt

A miss can only ever touch a node still **in** the active layer — i.e.
still on-screen and rewritable. Sealed strata are not in the tree to be
missed. Worst case a miss rebuilds one visible turn; it can never re-emit a
sealed row. The catastrophe (miss → re-emit → duplicate) is gone because
the sealed region is *physically absent from the diffable tree*.

### Why the seal count is always exact

The commit count is the summed height of whole **terminal** strata (whose
heights are stable once terminal), measured by maya's own layout engine at
maya's own width — the same render that just went to the wire — and capped
at `prev_rows - term_h`. There is no host height, no second measurement, no
width to drift against. Over-commit is impossible.

## The per-frame pipeline

1. **Reconcile** the active layer against the host's live node suffix
   (the nodes after the sealed frontier), reusing cached Elements whose
   `hash` is unchanged and calling `build()` only on a miss.
2. **Seal-first**: before composing, commit the *previous* frame's overflow
   — whole terminal strata that have scrolled past the *current* viewport —
   and drop them. This keeps the rendered canvas ≈ one viewport, so a stale
   re-anchor (height resize) never walks up into committed scrollback. This
   is the single commit point, and it is the direct analog of
   `agent_session` committing its frozen prefix.
3. **Resize recovery**, mirroring `Runtime::handle_resize` exactly:
   - **width change** → `demote_to_hard_reset` (the old-width scrollback is
     the terminal's; wipe + repaint the re-measured active layer fresh).
   - **height-only change** (the phone case) → `demote_to_stale`: case-(B)
     soft-repaints the viewport in place, re-anchoring maya's cursor model
     to the reflowed terminal **without a scrollback wipe**.
4. **Compose** the (reduced) active layer into one canvas via
   `render_tree_at`, one stratum per y-offset.
5. **Render** through the `InlineFrame` type-state (Empty → Fresh → Synced
   …), which emits the minimal per-row diff over the bounded active layer.

## Host API

```cpp
struct NodeRef { uint64_t key; uint64_t hash; bool terminal; };   // cheap POD
using BuildFn = std::function<Element(uint64_t key)>;             // only on a miss

FrameStats Strata::frame(span<const NodeRef> nodes, const BuildFn& build,
                         int term_cols, TermRows term_rows,
                         StylePool&, Writer&);
```

The host passes the FULL ordered node list every frame (two integers and a
bool each), plus a lazy builder. It issues **no `commit_scrollback`, keeps
**no `frozen_rows[]`, computes **no widths**. Strata owns the seam.

## Proof

A throwaway VT harness (continuous-buffer terminal model fed the *real*
bytes Strata emits) asserts the depositional invariant:

- **Streaming append + height-resize fuzz** (400 turns, soft-keyboard
  toggling): every emitted row appears **exactly once** across
  (scrollback ∪ screen) — **0 duplication, 0 loss**.
- **Width-change fuzz**: 0 duplication (HardReset scrollback-wipe on a width
  change is correct).
- **100k-node cold start**: 18 `build()` calls, 0 on resize, active layer
  ≈ one viewport — **O(viewport), independent of transcript length**.

## Status

Phase 0 (the renderer + proof) is complete and lands beside the existing
inline path without touching it. Phase 1 — porting `agent_session` and then
agentty's conversation onto Strata, deleting the host-side frozen prefix and
all `commit_scrollback` accounting — is the next step.
