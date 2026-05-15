# Render-path roadmap — what's left to do

Status snapshot of the render-correctness + slow-terminal-perf work
started from the structural-proposal review on `b9f493f`. This is a
working doc: items move out as they ship.

## Already shipped (visible in `main` log)

- Runtime canvas-clear fix in `app.cpp` (mirror of 489347b for the
  `run<Program>`/`run(event_fn,render_fn)` path that the original
  commit missed).
- Per-row `last_content_col` cache in `Canvas` — O(1) lookups
  replacing backward linear scans in `serialize`, `compose_inline_frame`,
  and `diff`.
- `render_tree_inline_autogrow` — single paint pass via two-compute
  measure→resize→recompute pattern. Closes the OOB cache window. See
  `docs/internals/inline-autogrow.md` for the failure-mode record.
- `Canvas::set_wide` — atomic paired wide-char write; orphan
  trail/lead unrepresentable in the API.
- `render::emit::erase_to_eol_safe` — centralised the
  SGR-reset-before-EL invariant in one header.
- `TextElement::format` returns `WrappedLine{text, byte_offset}` —
  drops the O(content × lines) substring scan in the painter's
  word-wrap+runs path.
- **A6**: `ScrollbackMarker` — typed token for `InlineFrameState`
  scrollback commits, clamped to live `prev_rows`. Forces callers to
  query current state instead of carrying a stale int.
- **B7**: `StylePool::write_transition_sgr` — differential SGR
  transitions, emits only the attribute toggles + fg/bg setters that
  changed between `prev_id` and `new_id`. ~50%+ byte reduction per
  style change on style-heavy content.
- **B1, B8**: bandwidth coalesce (`Writer::ns_per_byte` EMA +
  `compose_inline_frame(min_changed_rows)` hold-and-accumulate) —
  **removed**. The hold swallowed keystrokes on the typing path: a
  1-row composer change would meet the hold criterion and the first
  two keystrokes would only land when the third one forced a flush.
  Streaming pacing at ~20 Hz makes the saved syscalls negligible
  anyway. If a similar optimisation is ever needed it should live
  ABOVE compose (the caller decides not to render this tick) rather
  than inside it, where compose has no way to tell user input from
  animation.
- **B9**: `Sub::AnimationFrame{Msg}` — push-based animation
  subscription. Idle frames render zero bytes. The pull-based
  `request_animation_frame()` remains for backward compat.
- **B10**: `Canvas::clear` bounded by previous `max_y_+1` — ~10× less
  memory traffic per inline frame on a 500-row canvas with ~50 rows
  of content. The invariant ("cells beyond `max_y_` are blank") holds
  inductively. Originally caused a ghost, since root-caused and
  fixed via 88516a6 + 69688c5.
- **A1**: `prev_cells` shadow-of-wire — `compose_inline_frame`'s per-
  row loop writes the just-emitted slice into `prev_cells` in place
  (`[x_first_diff, x_last_diff+1]` for common rows, full row for
  new rows) instead of bulk-memcpying `[first_changed*W,
  content_rows*W]` at end of frame. Sparse-streaming frames drop
  from "copy every row below first_changed" to "copy only changed
  segments per row".
- **A2**: `CanvasStage{Drained, Painted}` runtime enum — transitions
  flipped by `set`/`fill`/`blit_packed_row`/`clear`/`clear_rows`/
  `resize`. Documents the paint lifecycle in one named field;
  compile-time type-state would template every Canvas user, so the
  runtime form is the shippable cut.
- **B5**: `intern_const(pool, style)` — defaulted-lambda-tag template
  so each call site instantiates a unique specialisation with its own
  static thread_local `cached_id`, keyed on `StylePool::pool_id()`
  (process-wide monotone counter, bumped on construction and on
  `clear()`). Skips the hash + slot-probe of `pool.intern()` on every
  paint after the first call.
- **Open ghost composer (b9f493f)**: fixed via `88516a6` (cache_id
  derived from owner_less-keyed weak_ptr identity, closes the
  pointer-recycling cache aliasing class) and `69688c5`
  (`Cmd::force_redraw()` / `Runtime::force_redraw()` host-triggered
  coherence collapse, mirrors `handle_resize`'s Divergent transition
  for first-input-after-streaming).

## Items deferred — open

### A3 — typed widget strips

The biggest item left, and the only structural piece still on the
table. Replaces the canvas-based painter with a strip-based composer.

**What it changes.** The renderer's intermediate representation today
is a 2D `Canvas` of packed cells. Widgets return Element trees, the
painter walks them and writes cells, the diff/serialize path reads
cells back to emit ANSI. A3 swaps the cell grid for `Strip` (one per
row, each a list of `(text, style)` runs already in wire-ready form).
The renderer never reads back — strips ARE what gets emitted.

The widget API (`Element`-returning functions) can stay; only the
internal pipeline changes. Cache layer goes from "cells per
`(cache_id, width)`" to "StripFrame per `(cache_id, width)`".

**Wins.**
- Closes the cache-capture bug class entirely. No 2D buffer to read
  out of bounds, leak stale cells from, or alias via recycled
  pointers. (All three mitigated by other fixes — see "open ghost
  composer" above — but A3 closes the class structurally rather than
  case-by-case.)
- Drops the "every cell starts blank each frame" load-bearing
  invariant. Strips are built fresh per frame; there's no shared
  surface to leak through.
- Style integration with the diff falls out naturally — B7's
  differential SGR was bolted onto a cell-comparison diff; with
  strips, style is part of the run, transitions live where the
  data does.
- Width-change invalidation is per-component instead of canvas-wide.
- Per-row strip equality + concat is cache-friendlier than packed-
  uint64 cell compare on sparse-change frames. (Marginal; we already
  SIMD-bulk-eq rows.)

**Costs.**
- Overlay / focus-ring / popup painting becomes awkward — currently
  trivial (write cells over existing ones).
- Wide-char and grapheme alignment moves from the canvas's explicit
  `width=1/2` markers into strip cursor-math.
- Every Element variant in the painter rewrites (TextElement,
  ComponentElement, BoxElement, BorderElement, ImageElement,
  ElementList, ElementListRef, FlexElement, ...). Every cache
  codepath rewrites. The diff and serialize paths get strip-based
  equivalents alongside the existing cell-based ones during the
  migration.
- Both paths must coexist behind a feature flag during the cutover
  — added complexity until the last codepath converts.

**Scope estimate.** 8-15 hours of focused work for the maya-internal
parts plus a multi-day soak in moha to shake out edge cases (wide
chars, layout-shift frames, scrollback boundary cases). Not a single-
session task.

**When to do it.** Open question. The bug class A3 closes has been
mitigated by other fixes; the wins are architectural insurance and
clean-up against future complexity (overlays, scene composition).
For maya's current target (chat-UI / agent-TUI use cases) the
existing architecture works. Revisit when:
- New requirements demand overlay / transparency / scene-level
  composition that the cell-grid model fights, OR
- Profiling identifies the canvas-cell intermediate as a real cost,
  OR
- A new cache-capture bug surfaces that the existing fixes don't
  cover.

When picked up, sequence:

1. Define `Strip` / `StyledRun` / `StripFrame` types behind
   `MAYA_STRIP_RENDERER=1` so both paths can coexist.
2. Strip composer for `TextElement` first (simplest leaf).
3. Composer for `ElementList` / `ElementListRef` (vstack concat).
4. Composer for `BoxElement` (flex children, h/v stack composition).
5. Composer for `ComponentElement` (preserve current cache shape but
   with StripFrame storage).
6. Composer for remaining Element variants in dependency order.
7. Strip-based `diff_strips` + inline-frame composer.
8. Wire into `Runtime::render` behind the flag.
9. Soak on moha. Convert tests.
10. Flip the default; remove the canvas path after a stability window.

### B4 — auto-measure within-frame cache

The current `component_cache` already memoizes rendered `child` per
`(cache_id, width)` across measure→paint within a frame, so for
cache_id'd components there's no double-render. What's still
duplicated is the `build_layout_tree` + `layout::compute` walk
between measure (`width=fixed, height=auto`) and paint (`width=fixed,
height=fixed`) — different constraints, so the cached layout tree
isn't reusable directly. A speculative win would be caching only the
tree-build (not the compute), which is 8-100 nodes per component;
likely sub-microsecond. Needs profile data before chasing.

## Order to pick up later

1. **A3** — structural cleanup; do it when the trigger conditions
   above hit. Not before.
2. **B4** — perf item, after profile data shows demand.
