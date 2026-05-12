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

## Open bug — composer/footer ghost

**Symptom**: while streaming markdown (or any UI where the footer is
positioned above the terminal foot), typing into the composer shifts
the footer downward to the terminal bottom and leaves a ghost copy of
the footer at its old position. Persists after streaming finishes.
**Fixed by terminal resize.**

**Diagnostic value of "fixed by resize"**: `handle_resize` collapses
both `fs_coherence_` and `in_coherence_` to `Divergent` AND bumps
`render_generation_`. Divergent wipes the `InlineFrameState`; the
generation bump invalidates `component_cache` pointer-keyed entries.
So the corrupting state is in one (or both) of:

- `InlineFrameState::prev_cells` desynced from terminal screen
  state.
- `component_cache` cell-region cache (`ComponentCacheEntry::cells`)
  holding cells captured for a different (component, width) than the
  current frame's blit target.

**Reproducibility checkpoint**: present at HEAD = `b9f493f`. Was NOT
caused by `bbc625b` / `aba6a95` / `9f69375` / `bb00a7c` (those four
were dropped via force-push, bug remains).

**Hypotheses to investigate** (none confirmed):

1. *Cursor-cap class*: at some intermediate frame, painted height
   exceeded `term_h`, `prev_rows` overshot the terminal, subsequent
   `compose_inline_frame` calls used `cursor_up` capped at
   `prev_on_screen - 1`, all emissions landed at the wrong physical
   row. Pre-existing for `inline.cpp::render_live`-style overflow
   scenarios; `inline-autogrow.md` describes this class.
2. *Content-keyed `cache_id` pointer collision* (element.hpp:229+):
   the implicit `shared_ptr<const Element>` → `Element` ctor derives
   `cache_id` from `sp.get()` (raw pointer). If a `shared_ptr` is
   freed and the allocator hands the same address to a new
   `shared_ptr`, the new ComponentElement gets a `cache_id` colliding
   with the cached entry of the dead one. Content-keyed lookup
   bypasses the generation check — old `cells` get blitted into a
   region painted for unrelated content. Fix candidates:
   `std::owner_hash`-derived id, or weak_ptr stored in the cache
   entry for liveness check.
3. *Streaming-markdown prefix `ComponentElement` mutation*: the
   tail-only fast path in `StreamingMarkdown::build` mutates
   `cached_build_.children.back()` in place. If the *prefix*
   ComponentElement's lambda captures a `prefix_` that gets replaced
   between frames in an unexpected order, the cache could return
   cells for a stale prefix while the new lambda renders new content.

**Next step when we pick this up**: instrument with
`MAYA_FRAME_PROF` to capture per-frame `prev_rows` / `content_rows` /
`term_h` and the (cache_id, width) of every cache hit during the
reproducer. Diff against a clean session. If hypothesis (1), see
shadow-of-wire (A1) below. If (2), patch the implicit ctor's cache_id
derivation.

## Items deferred from the original proposal

Each is independently shippable; ordering reflects expected value /
risk ratio.

### A6 — typed `ScrollbackMarker` token for `commit_prefix`

Shipped earlier as `bbc625b`, dropped in the force-push. Low risk,
small additive change (kept `commit_prefix(int)` for source compat).
Re-applying is straightforward; deferred only because the marginal
safety win is small without an open bug it directly addresses.

### B7 — differential SGR transitions

Shipped earlier as `aba6a95`, dropped in the force-push. Real
slow-terminal win on style-heavy content (markdown, syntax
highlighting): emits only the changed attributes per transition
instead of the cached full `\x1b[0;...m` reset. Wire bytes change but
tests are property-based. Re-applying is straightforward; falls back
to full reset when `prev_id == UINT16_MAX` so terminal-state-unknown
emissions are safe.

### B10 — bounded canvas clear by previous `max_y_+1`

Shipped earlier as `9f69375`, reverted as `bb00a7c` (caused a ghost
the user observed). The invariant ("cells beyond `max_y_` are blank")
looked watertight on paper. Three plausible interactions with
`last_col_`, full-canvas `damage_`, or the cell-region cache need
isolating before re-attempting. If we revisit, do it AFTER the open
ghost bug is understood — they may share a root cause.

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

### B1 + B8 + B9 — bandwidth budget, coalescing, animation-as-sub

Multi-piece Runtime-layer work. The most direct "slow terminals get
faster" win.

- **B1**: measure write latency in `Writer`, expose a budget signal.
- **B8**: when a frame's diff is below `min_changed_cells` threshold,
  hold for one more tick and coalesce.
- **B9**: replace `request_animation_frame` pull with a
  `Sub::AnimationFrame(Msg)` subscription so idle frames render zero
  bytes.

These three are designed to compose: the budget tells coalescing
what threshold to use; animation-as-sub lets the budget actually
downgrade frame rate without the application knowing. Each piece is
moderate effort, integration risk is in the Runtime loop ordering.

### A3 — typed widget strips

Explicitly deferred in the original proposal. The biggest refactor;
changes the widget contract from "Element-returning function" to
"Strip-producing function" so the renderer never reads back from the
canvas. Closes the OOB cache-capture class entirely (rather than
just the autogrow window already handled by B2). Don't tackle until
A1 + B10 are settled — those share an invariant that A3 builds on.

## Order to pick up later

1. **Reproduce + diagnose the ghost composer** with profiling
   instrumentation. Until we know the root cause, more refactors
   risk piling onto a latent bug.
2. **A6** + **B7** — low-risk re-applications of the dropped commits.
3. **B1 + B8 + B9** — slow-terminal payoff; can land in pieces.
4. **B10 retry** only after the ghost composer root cause is
   understood (likely shares the underlying state divergence).
5. **B4 / B5** — perf items, after profile data shows demand.
6. **A1 / A2 / A3** — structural refactors, biggest scope, lowest
   urgency now that the open bug is being chased separately.
