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
  SGR-reset-before-EL invariant in one header. Three inlined emit
  sites now route through it.
- `TextElement::format` returns `WrappedLine{text, byte_offset}` —
  drops the O(content × lines) substring scan in the painter's
  word-wrap+runs path.
- **A6**: `ScrollbackMarker` — typed token for `InlineFrameState`
  scrollback commits. Constructed via `scrollback_marker(rows)` which
  clamps to live `prev_rows`; consumed by `commit(marker)`. Forces
  callers to query current state instead of carrying a stale int.
  `commit_prefix(int)` retained for source compat.
- **B7**: `StylePool::write_transition_sgr` — differential SGR
  transitions. Emits only the attribute toggles + fg/bg setters that
  changed between `prev_id` and `new_id` instead of the full
  reset-and-set form. ~50%+ byte reduction per style change on
  style-heavy content (syntax highlighting, markdown runs). Falls back
  to the cached full sequence when `prev_id == UINT16_MAX` so
  terminal-state-unknown emissions still land in a known SGR config.
- **B1**: `Writer::ns_per_byte()` — EMA of wire latency measured
  across each `write_all` syscall loop. Drives B8's coalesce threshold:
  fast ttys (low ns-per-byte) pass `min_changed_rows = 0` and behave
  exactly as before; slow ttys (ssh hop, PuTTY) raise the threshold to
  2 or 4 so a streak of tiny diffs coalesces into one larger flush.
- **B8**: `compose_inline_frame(min_changed_rows = N)` — when the
  row-diff scan finds `changed_rows ∈ (0, N]` AND `content_rows ==
  prev_rows`, the function returns with `out` and `state` untouched so
  the next frame's diff accumulates against the same `prev_cells`.
  Bounded by `InlineFrameState::held_count ≤ kMaxConsecutiveHolds=2`
  so a perpetual tiny diff still surfaces within ~32ms.
- **B9**: `Sub::AnimationFrame{Msg}` — push-based animation
  subscription. The model declares "I want a tick each frame while
  this is in my subscription" and stops including it when the animation
  settles. The runtime pumps collected msgs at
  `kAnimationFrameInterval` cadence. Idle frames (no AnimationFrame
  sub present) render zero bytes. The pull-based
  `request_animation_frame()` deadline path remains for backward
  compat.
- **Open ghost composer (b9f493f)**: fixed via two converging changes —
  `88516a6` (cache_id derived from owner_less-keyed weak_ptr identity,
  closes the pointer-recycling cache aliasing class) and `69688c5`
  (`Cmd::force_redraw()` / `Runtime::force_redraw()` host-triggered
  coherence collapse, mirrors `handle_resize`'s Divergent transition
  for first-input-after-streaming). The agentty host wires
  `force_redraw` on stream-settled → next-user-input edges.

## Items deferred — open

Each is independently shippable; ordering reflects expected value /
risk ratio.

### B10 — bounded canvas clear by previous `max_y_+1`

Shipped earlier as `9f69375`, reverted as `bb00a7c` (caused a ghost
the user observed at the time). The originating ghost composer bug
has since been root-caused and fixed (88516a6 + 69688c5 above), so
B10 is now safe to retry. Still risky in isolation — the full
`canvas.clear()` per frame is now a load-bearing invariant
(489347b's commit message: "every cell starts blank each frame ...
eliminates the whole stale-cell-leak class"). If we revisit, do it
behind a feature flag with a one-week observation window before
flipping the default.

### B5 — compile-time style → permanent style_id

Per-pool cache identity is tricky (pools are per-render-state, can be
destroyed and recreated). Marginal perf — `StylePool::intern` is
already O(1) on a hash table with pre-cached SGR. Reach for this
only after a profile shows intern as a hot path.

### B4 — auto-measure unconditionally caches rendered child within frame

The current cache already covers stable-pointer + cache_id'd
components. The double-render claim from the original proposal
applies only to ephemeral components without `cache_id` — and those
are caught by within-frame pointer caching too. Needs a profile
before chasing.

### A2 — type-state `Canvas<Drained>` → `<Painted>` → `<Diffed>`

True compile-time type-state would rewrite every Canvas user — every
widget, every `paint_element`, every `canvas_run` example. Too
API-invasive for a single commit. If we want progress here, the
shippable form is a runtime stage enum + debug assertions; promote
to compile-time later.

### A1 — `prev_cells` becomes shadow-of-wire

Per-emit shadow update in `emit_cell_run` + `compose_inline_frame`
instead of the end-of-function bulk memcpy. Semantic clarity + perf
on sparse-streaming frames. Doesn't add new anti-ghost guarantees on
top of what's already shipped (the unconditional clear closes the
489347b-class bug at the painter layer). Skipped per direction.

### A3 — typed widget strips

Explicitly deferred in the original proposal. The biggest refactor;
changes the widget contract from "Element-returning function" to
"Strip-producing function" so the renderer never reads back from the
canvas. Closes the OOB cache-capture class entirely (rather than
just the autogrow window already handled by B2). Don't tackle until
A1 + B10 are settled — those share an invariant that A3 builds on.

## Order to pick up later

1. **B10 retry** — the original blocker (ghost composer) is now
   root-caused and fixed; B10's invariant ("cells beyond `max_y_+1`
   are blank") deserves a fresh look behind a feature flag.
2. **B4 / B5** — perf items, after profile data shows demand.
3. **A1 / A2 / A3** — structural refactors, biggest scope, lowest
   urgency now that the open bug is fixed and B1+B8+B9 deliver the
   bulk of the slow-terminal payoff.
