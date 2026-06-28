# Strata overflow deposition — the windowed-compose fix

**Status:** FIXED. The deposition invariant holds under the adversarial
VT-model fuzz (`tests/test_strata.cpp`) for the full hostile op mix —
including a single turn taller than the viewport and drastic height shrinks,
the cases the original whole-node seal could not handle. This note records the
bug that was found, the fix that closed it, and how the harness proves it.

---

## TL;DR

Strata's deposition invariant — *"every emitted row appears exactly once
across (native scrollback ∪ screen)"* — used to **not hold when the active
band was taller than the viewport.** The band is intentionally kept at
`keep_viewports` (default **3**) viewports of content for resize headroom, but
the *whole* band was composed into a single inline frame and handed to the
`InlineFrame` renderer. When that band exceeded `term_rows`, the renderer
scrolled the overflow into the terminal's native scrollback — yet `band_`
still held those rows and **re-emitted them on the next frame**, stamping a
duplicate against their own immutable scrollback copy.

The fix: **compose a WINDOWED active layer.** Strata keeps the full band for
resize re-measure, but the canvas it hands the renderer excludes rows that
have already deposited into native scrollback. The renderer therefore can
never see — let alone re-emit — a row the terminal already owns.

## The fix, precisely

A single new piece of state, `committed_rows_` (in `Strata`), is the count of
band rows, from the top, that have physically deposited into native
scrollback. Each frame:

1. **Window the compose (Section 3).** The canvas is band rows
   `[committed_rows_, band_height)`. Each node paints at `y_global -
   committed_rows_`; a node entirely above the window is skipped, and the one
   node *straddling* the window top is clipped to its still-visible tail by
   painting it at a negative origin (`render_tree_at` clips above row 0). So
   `win_h = band_height − committed_rows_`, and only still-mutable rows (plus
   this frame's fresh growth) are ever in the canvas.
2. **Render** drives the `InlineFrame` machine exactly as before, over the
   windowed canvas.
3. **Advance the watermark (Section 5).** The render just scrolled the top
   `max(0, win_h − term_rows)` rows of the windowed canvas past the top edge.
   Strata adds them to `committed_rows_` so next frame's window excludes them,
   and shrinks the renderer's shadow by the same count via `commit(marker)`,
   keeping `prev_rows ≤ term_rows`. The renderer never holds, addresses, or
   re-diffs a deposited row again.
4. **Drop fully-deposited front nodes (Section 2).** When `committed_rows_ ≥
   band_.front().height`, that whole node is in scrollback — drop it and
   decrement `committed_rows_`. Pure bookkeeping; advances `sealed_count_`.

This is the row-granular analog of the old whole-node `seal_front`. It is
correct *by construction*: the canvas the diff sees can only contain rows that
are still on screen or scrolling off **for the first time** this frame.

### Resize / rewind / swap recovery

- **Width change** → hard reset (wipe + repaint fresh); the old-width
  scrollback belongs to the terminal. `committed_rows_ = 0`.
- **Height change** → soft repaint (case B, no scrollback wipe). `committed_rows_`
  is raised monotonically to `max(committed_rows_, band_height − term_rows)`:
  a deposited row can never be un-scrolled, so a height *grow* keeps the
  watermark (the window just shrinks below the viewport) and a height *shrink*
  raises it so the newly-overflowing rows are excluded and the case-B emit
  covers the whole window with no stale rows above.
- **Rewind** (live node count dropped) → same soft repaint, so the on-screen
  rows above the new, shorter band are erased.
- **Live-node REFLOW of an already-deposited row** → hard reset. The deposit
  watermark is monotonic and row-granular, so a LIVE (non-terminal) node's
  top rows can be committed into native scrollback the instant they scroll
  past the fold. But a live node still reflows — a streaming paragraph
  re-wraps, the reveal clip retracts — so if a *deposited* row's content
  later changes, the scrollback copy is frozen STALE and every later windowed
  canvas excludes it. The terminal owns that scrollback row: it can be
  neither corrected in place nor re-emitted (a re-paint double-stamps it), so
  the only duplicate-free recovery is the same wipe-and-repaint hard reset
  the height-shrink path uses. Strata detects it precisely — it records the
  `{key, hash}` of the node owning the last committed row each frame and arms
  the reset ONLY when that node is still non-terminal **and** its hash changed
  — so a giant live node that merely overflows every frame (its scrolled-off
  head is already-emitted, byte-stable streamed content) does NOT trigger it.
- **Wholesale swap** is caught by *two* detectors now: the original
  frontier-fingerprint check, plus a **band-anchor** check — the front
  live node must reappear in the host's list every frame **at the sealed
  frontier** (index `sealed_count_`). A swap breaks this two ways and both
  arm a hard reset: (a) the anchor key VANISHES (thread switch / rewind
  below the band), or (b) the anchor key is still present but lands at an
  index `> sealed_count_` — brand-new nodes were inserted BEFORE the live
  band, which the append-only contract forbids. Case (b) is essential
  because the host's bottom chrome node uses a **session-constant key**
  (it is present in EVERY surface's node list), so a plain "key present?"
  test let a welcome(1 node)→thread(N nodes) swap slip through and the
  whole new transcript was steady-reconciled + deposited in one frame (the
  "renders the whole thread on load" bug). Comparing the anchor's POSITION
  catches it: the LIVE node jumped from index 0 to N-1 with N-1 new nodes
  ahead of it. The band-anchor check is what keeps swap detection working
  now that windowing rarely advances `sealed_count_`.

## How it was found

`tests/test_strata.cpp` builds a faithful VT terminal model (`vt::Term`) with
a real scrollback/screen split, drives `Strata::frame()` through a seeded,
interleaved hostile op stream (stream-grow / shrink / reflow / seal / append /
wide-burst / resize-width / resize-height / thread-swap / rewind, with turns
allowed to grow taller than the viewport), feeds the **real emitted bytes**
through the model, and after every frame asserts each unique content marker
appears **at most once** across `(scrollback ∪ screen)`, that every live-tail
row is on screen, and that an on-screen live row shows its **current** text.

Against pristine (pre-fix) Strata the invariant broke within a few hundred
frames on most seeds. With the windowed compose it survives the full sweep:

```sh
cmake -B build -DMAYA_BUILD_TESTS=ON -DMAYA_FAST_TESTS=ON
cmake --build build --target test_strata -j
./build/test_strata 64 8000        # 64 seeds × 8000 frames → ALL PASS
./build/test_strata 1 4000 1       # seed 0, verbose (frame trace + byte dumps)
```

`test_strata` is now registered with `ctest` (bounded to `12 2000` for CI
speed); run the deeper sweep by hand.

## Files

- `tests/test_strata.cpp` — the harness, VT model, fuzzer, and triage tooling.
- `src/render/strata.cpp`, `include/maya/render/strata.hpp` — the renderer;
  the windowed compose lives in `frame()` Sections 2/3/5 and the
  `committed_rows_` member.
- `docs/internals/strata-renderer.md` — the design essay.
