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

### Why the deposit count is always exact

Strata composes a **windowed** active layer: it keeps `keep_viewports` of
content live (for width re-measure) but the canvas it hands the renderer
excludes rows that have already deposited into native scrollback, tracked by a
`committed_rows_` watermark. The deposit count each frame is therefore just
`max(0, win_h - term_h)` — the rows the renderer physically scrolled off the
top edge, measured by maya's own layout engine at maya's own width, the same
render that just went to the wire. There is no host height, no second
measurement, no width to drift against, and no whole-node boundary to round
to: deposition is **row-granular**, so even a single turn taller than the
viewport is handled. Over-commit is impossible — the renderer never sees a
row the terminal already owns.

## The per-frame pipeline

1. **Reconcile** the active layer against the host's live node suffix
   (the nodes after the sealed frontier), reusing cached Elements whose
   `hash` is unchanged and calling `build()` only on a miss.
2. **Deposit bookkeeping**: drop whole front nodes that have fully deposited
   (pure accounting — they are in scrollback, not in the canvas). The
   `committed_rows_` watermark, advanced at the end of the *previous* frame,
   is what keeps the composed window ≈ one viewport, so a stale re-anchor
   (height resize) never walks up into committed scrollback.
3. **Resize / rewind / swap recovery**, mirroring `Runtime::handle_resize`:
   - **width change** → `demote_to_hard_reset` (the old-width scrollback is
     the terminal's; wipe + repaint the re-measured active layer fresh).
   - **height change or rewind** → `demote_to_stale`: case-(B) soft-repaints
     the viewport in place, re-anchoring maya's cursor model **without a
     scrollback wipe**. `committed_rows_` rises monotonically (a deposited
     row can never un-scroll).
   - **wholesale swap** is auto-detected (frontier fingerprint **and** a
     band-anchor key check) and self-arms a hard reset.
4. **Compose the WINDOWED active layer** — band rows `[committed_rows_,
   band_height)` — into one canvas via `render_tree_at`, each stratum at
   `y_global - committed_rows_` (the straddling node is clipped to its
   visible tail).
5. **Render** through the `InlineFrame` type-state (Empty → Fresh → Synced
   …), which emits the minimal per-row diff over the bounded window, then
   **advance `committed_rows_`** by the rows just scrolled off and shrink the
   renderer's shadow to match (`commit(marker)`), so a deposited row is never
   addressed again.

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

An adversarial, reproducible VT-model fuzz oracle (`tests/test_strata.cpp`,
registered with `ctest`) is the second independent party Strata's design
removed from the renderer — re-introduced in the test only. A faithful
terminal model (`vt::Term`) with a real scrollback/screen split consumes the
**real bytes** Strata emits and, after every frame, asserts the depositional
invariant directly: every unique content marker appears **exactly once**
across (scrollback ∪ screen).

A deterministic seeded fuzzer drives Strata through interleaved hostile
operations — streaming append, turn sealing, new turns, wide/emoji content,
height **growth and drastic shrink**, **a single turn taller than the
viewport**, transcript rewinds, and wholesale thread swaps. The invariant
holds across the full sweep (64 seeds × 8000 frames = ~512k frames, and the
same at the widened op mix). On failure the harness prints the failing phase,
the duplicated marker, both buffer locations, and the exact escape bytes of
the last frames.

```sh
./build/test_strata 64 8000      # ALL PASS — deposition invariant held under fuzz
```

## Status

The renderer + the windowed-compose deposition fix + the fuzz proof are
complete and land beside the existing inline path without touching it. The
next step — porting `agent_session` and then agentty's conversation onto
Strata, deleting the host-side frozen prefix and all `commit_scrollback`
accounting — is now unblocked.
