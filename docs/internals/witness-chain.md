# The Witness Chain — Type-theoretic Scrollback Integrity

This is the design and proof obligations for maya's inline-mode
correctness guarantee. The theorem we prove (modulo stated
assumptions) is **Scrollback Integrity**:

> For any well-typed maya program `P`, for any sequence of host events
> `e₁…eₙ`, the bytes that the terminal emulator commits to its native
> scrollback are exactly the bytes `serialize` would produce for the
> overflow-portion of past frames, in order. No row is ever targeted
> by an escape sequence emitted by maya once it has overflowed the
> viewport.

This document explains how the type system witnesses that claim. The
implementation lives in:

- `include/maya/render/wire_row.hpp`     — Layer 1
- `include/maya/render/serialize.hpp`    — Layer 2 (`ContentRows`)
                                           and Layer 3 (`ShadowWitness`)
- `include/maya/render/canvas_witness.hpp` — Layer 0 (canvas caches)
- `include/maya/render/frame_bytes.hpp`  — Layer 4
- `include/maya/render/inline_frame.hpp` — Layer 5 (apex)
- `include/maya/render/scrollback_ledger.hpp` — Layer 6 (trim accounting)

## The theorem in five parts

The integrity claim decomposes into five sub-claims, each closed by a
distinct layer of the chain.

**T0 (canvas-cache integrity).** For every `Canvas` value reaching
`diff()`, the cached metadata — `last_content_col(y)` and
`max_content_row()` — is an exact function of the cell buffer. No diff
is ever computed against a canvas whose trailing-blank extent is
stale-high (the historical "shorter header painted over longer; old
tail not cleared" ghost-row class).

> *Proof.* `diff(CanvasWitness&&, CanvasWitness&&, ...)` is the only
> witness-gated diff entry point; the legacy raw-Canvas overload is
> kept solely so the existing 20+ scrollback tests keep exercising the
> production path while the migration bakes in. `CanvasWitness` is
> move-only with a private constructor friended only to
> `verify_canvas`. `verify_canvas` re-derives `last_col_[y]` and
> `max_y_` from `cells_` (slow O(W·H) scan) and returns `nullopt` on
> disagreement — the host's only recovery is `clear_row()` + repaint,
> never "emit anyway." On match it records FNV-1a hashes of both the
> cells buffer and the (last_col_, max_y_) tuple. Inside `diff()`,
> both hashes are recomputed against the moved-in canvases and
> compared with the witness-recorded values; mismatch triggers
> `std::abort` (memory-corruption / use-after-free signal,
> single-threaded renderer). Therefore every byte that `diff()` emits
> is computed against a canvas whose cached metadata is true at the
> moment of emit. ∎

**T1 (no-ghost).** The renderer emits no cursor-positioning escape
sequence that targets a row above the viewport top.

> *Proof.* Every cursor move passes through `emit_move(out, from, to)`
> in `wire_row.hpp`. Both arguments are `WireRow`s. The only producers
> of `WireRow` are `Viewport::row(y)` (saturating), `Viewport::top_row`
> / `bottom_row`, and `WireRow::up(n)` / `down(n)` (saturating). The
> saturation is built into `WireRow`'s private constructor via
> `std::clamp(y, top, bottom-1)`. There is no public constructor that
> takes an unchecked `int`, no `data()` accessor, no other escape
> hatch. The set of representable values is `[top, bottom)`. ∎

**T2 (no-corruption).** The renderer emits no cell-paint escape
sequence whose target cell disagrees with `paint(view(model))` at the
corresponding canvas coordinate.

> *Proof.* `compose_inline_frame_v2` requires `ShadowWitness&&` as a
> typed argument. The only producer of `ShadowWitness` is
> `verify_shadow(state)`. The witness records the FNV-1a hash of
> `state.prev_cells` at issue time. Inside compose, the hash is
> recomputed and compared against the witness's recorded value; any
> mismatch triggers `std::abort` (memory-corruption signal). Since
> `ShadowWitness` is move-only with a private constructor friended
> only to `verify_shadow`, no call site can construct one without
> first having proven `state.shadow_hash == FNV(state.prev_cells)`.
> The `prev_cells` field itself is never mutated outside compose
> (Layer 5 makes the field a private member of `InlineFrame<Tag>` and
> friends only the transitions that legitimately advance the cell
> buffer). Therefore the cells compose diffs against are exactly the
> cells the terminal is currently displaying, and the emitted bytes
> are the byte-for-byte difference. ∎

**T3 (monotone scrollback).** Once a row leaves the viewport via a
`\r\n` overflow, no subsequent render targets it.

> *Proof.* Cursor movement is closed under `Viewport` (T1). Scrollback
> advance happens only through `commit(ScrollbackMarker)`, which
> takes a marker minted from the live state via `scrollback_marker(rows)`.
> The marker's row count is clamped to `[0, prev_rows]` at issue
> time; the marker is consumed once (move-only token); the resulting
> state has `prev_rows` strictly less than or equal to the prior.
> The set of row coordinates referenced by future cursor moves is
> bounded above by `prev_rows_at_emit + W` where `W` is `wire_cursor_rows`
> from the most recent successful emit. Therefore no future emit
> targets a row index that has overflowed the viewport at any prior
> emit. ∎

**T4 (trim accounting).** For any front-trim of a host's sealed inline
prefix, the row count committed to the shadow
(`commit_inline_prefix`) equals the number of physical rows the
dropped blocks occupied on the wire in the previous frame.

> *Proof.* A ledger host holds its sealed prefix in a
> `ScrollbackLedger` and renders it through `ledger_ref` (or
> `Conversation::Config::ledger`). The paint pass records every
> block's laid-out height into the ledger each frame — the SAME
> `layout::compute` pass, at the SAME width, in the SAME frame as the
> bytes that reach the wire; there is no second measurement pipeline
> to drift. `drop_front()` accrues the dropped blocks' paint-recorded
> heights as debt, gated on provability: a block whose height has
> never been recorded by a ledger-tagged paint (`recorded < 0`) stops
> the drop walk — a block becomes droppable only after one paint has
> recorded the height the commit will use, so an under-commit of an
> unpainted-but-physical block is unrepresentable. `harvest()` mints
> the accumulated debt as a `ScrollbackDebt`, whose constructor is
> private to the ledger; `Cmd::commit_scrollback(ScrollbackDebt)` is
> the only consumer. The host can only transport the token — it
> cannot fabricate, scale, or "adjust" a count. (The raw-int overload
> survives for non-ledger hosts, `[[deprecated]]`, and is clamped to
> the provable overflow inside `commit_inline_prefix` regardless.)
> Width-change race: if a trim lands between a width change and the
> next paint, the minted debt reflects the old width — safe, because
> a width change demotes inline coherence off `Synced` and
> `commit_inline_prefix` is a no-op in any non-Synced state; the debt
> evaporates against a frame that repaints from scratch. ∎

Together, T0+T1+T2+T3+T4 imply Scrollback Integrity:

- T0 ensures the cells `diff()` reads, and the trailing-blank cache
  the EL optimisation trusts, are coherent with one another — closing
  the "stacked header / stale tail" ghost class.
- T1 keeps the cursor inside the viewport, so no escape sequence
  reaches above the live region.
- T2 ensures whatever is written to the viewport is the truth (matches
  the canvas).
- T3 ensures the cumulative effect of overflows commits exactly the
  truth that was on the viewport at the moment of overflow.
- T4 ensures a host-initiated trim of the sealed prefix advances the
  shadow by exactly the rows the dropped content physically occupied —
  closing the stranded-duplicate / eaten-tail ghost classes that every
  historical host-side trim bug belonged to.

The bytes in emulator scrollback at any moment are the union of:
truths-committed-so-far + truth-currently-on-screen. Both are
identical to `paint(view(model_at_that_time))` at the relevant
coordinates. ∎

## Stated assumptions (hypotheses of the theorem)

Honest type theory names its hypotheses. Maya's claim holds *only* when
all four hold:

1. **A1 — Terminal emulator conformance.** The terminal honors
   ECMA-48 cursor control codes, DEC mode 2026 if advertised, EL, ED,
   DECAWM. A buggy emulator that misinterprets `CSI A` as something
   other than cursor-up violates the assumption. Out of scope.

2. **A2 — Kernel write fidelity.** `write(2)`'s return value accurately
   reflects bytes delivered to the kernel's tty buffer. Bytes that the
   kernel reports as delivered are not corrupted in transit. This is a
   POSIX guarantee; treat as axiomatic.

3. **A3 — Exclusive fd ownership.** Maya's `Writer` is the only entity
   in the process writing to fd 1. Host code that calls `std::cout` or
   `fwrite(stdout)` outside `Cmd::print` or `Runtime::with_external_io`
   bypasses the entire chain. Documented invariant; cannot be enforced
   by the type system because C++ does not let us seal `stdout`.

4. **A4 — No silent memory corruption.** Between `verify_shadow`
   returning a witness and `compose_inline_frame_v2` consuming it,
   the `prev_cells` buffer is not mutated by an unrelated bug (use-after-
   free, buffer overrun in adjacent code, cosmic ray). The FNV-1a
   re-hash inside compose catches mutation with probability `1 - 2⁻⁶⁴`.
   Hardware-detected memory errors are out of scope.

## The layers in detail

### Layer 1 — `WireRow` + `Viewport`

Saturating cursor coordinates. `Viewport::for_fresh_frame(term_h)` and
`Viewport::for_redraw(term_h, wire_cursor_rows)` are the two public
factories; both clamp `term_h` to ≥ 1 and assign a unique `ViewportId`.
`Viewport::row(int)` is the sole producer that accepts an arbitrary
absolute row index, and it clamps via `std::clamp`. `WireRow::up(n)`
and `down(n)` return rows clamped inside the same `[top, bottom)`
range. `emit_move(out, from, to)` is the sole emitter of cursor-up /
cursor-down escapes.

This closes T1. The set of cursor targets that any `emit_move` call
can produce is precisely the set of `WireRow` values, and the set of
`WireRow` values is precisely `Viewport::[top, bottom)`.

### Layer 2 — `ContentRows`

`int`-wrapper with a private constructor. Sole producer:
`content_rows(canvas)`, which calls `content_height(canvas)` and binds
the resulting `int` to the canvas pointer. Future overloads of
`compose_inline_frame` consume `ContentRows`. The historical bug class
("computed row count from the wrong source" — layout's height, a
hard-coded constant, a stale `prev_rows`) becomes uncompilable: no
call site can construct a `ContentRows` from anything but the canvas
the renderer will paint.

### Layer 3 — `ShadowWitness`

Move-only token. Sole producer: `verify_shadow(state) -> std::optional<
ShadowWitness>`. Returns `nullopt` iff the FNV-1a hash of
`state.prev_cells` no longer matches `state.shadow_hash` (the value
the last successful compose recorded). On match, returns a witness
binding the state pointer and the hash value.

`compose_inline_frame_v2` consumes `ShadowWitness&&`. Inside compose,
the hash is recomputed against the freshly-moved state and compared
with `witness.hash_at_issue()`. Mismatch is unrecoverable
(`std::abort`); since the renderer is single-threaded, the only way
the hash can change between two consecutive function calls is
memory corruption.

This closes T2's "shadow matches wire" precondition.

### Layer 4 — `FrameBytes`

Capsule type returned by `compose_inline_frame_v2`. Carries:

- `std::string bytes_` — the byte stream the frame would emit.
- `InlineFrameState successor_` — the state value the caller should
  hold *if* the bytes ship.

Move-only. The only ways to consume a `FrameBytes`:

- `commit_to(writer) && -> CommitOutcome` — variant of
  `commit::Synced{state}` (full delivery) or `commit::Stale{state, reason}`
  (hard I/O error).
- `abandon() && -> commit::Stale{state, ok()}` — explicit drop.

The variant is mandatory: structured bindings on `std::variant` are
ill-formed, so every call site must `std::visit` both arms. There is
no "success-only" code path; every consumer handles both outcomes.

The state advance and the byte delivery are fused into one operation
whose outcome is a value the caller must destructure. The historical
bug ("state advanced; write returned partial; next compose diffed
against a state the wire doesn't show") becomes a compile error because
the *only* way to obtain a `Synced` state is the return value of a
successful `commit_to`.

### Layer 5 — `InlineFrame<Tag>`

Phantom-tagged type-state. Five tags: `Empty`, `Fresh`, `Synced`,
`Stale`, `Sealed`. Each is a distinct class (template specialization);
each is move-only; each has a private constructor friended only to the
predecessors in the transition graph:

```
                           seed()
   InlineFrame<Empty>  ────────────▶  InlineFrame<Fresh>
   InlineFrame<Fresh>  ──render────▶  InlineFrame<Synced> | <Stale>     (case A)
   InlineFrame<Synced> ──verify────▶  optional<ShadowWitness>
   InlineFrame<Synced> ──render────▶  InlineFrame<Synced> | <Stale>     (Synced + witness)
   InlineFrame<Synced> ─demote_to_stale─▶ InlineFrame<Stale>
   InlineFrame<Synced> ──commit(marker)─▶ InlineFrame<Synced>           (scrollback advance)
   InlineFrame<Stale>  ──render────▶  InlineFrame<Synced> | <Stale>     (case B)
   InlineFrame<*>      ─finalize(out)──▶ InlineFrame<Sealed>
```

`InlineFrame<Synced>::render` requires `ShadowWitness&&` as an rvalue
parameter — you cannot render a Synced state without first calling
`verify()`. `InlineFrame<Sealed>` has no `render` method, so
rendering after `finalize` is a compile error.

The underlying `InlineFrameState` is held by value as a private member.
It is moved through every transition; the chain is linear.

### Layer 6 — `ScrollbackLedger` + `ScrollbackDebt` (trim accounting)

The layers above close what MAYA emits. Layer 6 closes the one number
a HOST is still trusted to supply: how many rows a front-trim of its
sealed prefix sheds. Historically the host computed that itself — a
parallel measurement pipeline (element re-measure at a
host-reconstructed width, resize healing, N bookkeeping vectors kept
in lockstep by discipline) — and every historical trim-corruption bug
was drift between that pipeline and what maya painted: a 2-column
wrap-width mis-reconstruction under-counting on narrow terminals (the
phone-over-SSH duplication ghost), a stale post-resize height stamp
over-committing and re-scrolling the visible tail, a one-row estimate
drift dropping an on-screen entry.

Two moves—plus a third that closes the seal instant—retire the class
by construction:

1. **Maya measures, not the host.** `ScrollbackLedger` owns the sealed
   blocks (elements + per-block meta in one structure). Rendering goes
   through `ledger_ref(ledger)` — an `ElementListRef` tagged with the
   ledger — and the paint pass stamps each block's laid-out height
   back into the ledger every frame: same layout pass, same width,
   same frame as the wire bytes. Recorded heights self-heal across
   resize within one paint. Seal-time estimates exist ONLY for trim
   policy (`row_total()` / `block_rows()`); they never feed debt.

2. **The commit count is a typed token.** `drop_front()` accrues the
   dropped blocks' recorded heights as debt — gated so only
   paint-recorded blocks may shed rows, and quantized so a separator
   never leads the remaining prefix. `harvest()` mints a
   `ScrollbackDebt` (private constructor, friended only to the
   ledger); `Cmd::commit_scrollback(ScrollbackDebt)` is the sole
   consumer. Fabricating or adjusting a commit count no longer
   typechecks; the raw-int overload is `[[deprecated]]` and clamped
   inside `commit_inline_prefix` regardless.

3. **Maya measures the freeze instant too** (`seal_measured`). The
   one measurement left on the host side after moves 1–2 was the
   seal-time layout that warms the renderer's hash-keyed measure
   cache so the freeze frame is byte-stable (a block sealed at the
   freeze instant carries the hash_id the live tail stamped, and the
   measure path trusts a cached height blindly — a stale live-phase
   entry would transiently shrink the tree vs prev_cells and fire the
   render gate). Hosts ran that layout at a width THEY reconstructed
   ("terminal columns minus the chrome paddings I believe wrap my
   fragment") — the same drift class as the accounting, at the seal
   site. Now the paint pass records the ACTUAL width constraint
   `layout::compute` handed the ledger's fragment
   (`record_paint_width`), and `seal_measured()` lays the new block
   out at exactly that width, inside a scoped `RenderContext` pinned
   to it (so the component auto-measure clamp agrees with the paint
   pass at any terminal size). The measured height doubles as the
   policy estimate until the first ledger paint records the real
   value. Accounting still never touches it.

Sole producers, sole consumers, move-only tokens — the same discipline
as Layers 1–5, applied to the host boundary. The residual host freedom
is policy only: WHEN to trim and HOW MUCH context to keep. A sloppy
policy estimate can trim too much or too little *content*; it
structurally cannot corrupt scrollback.

## What the test surface looks like

Two test files witness the chain at the type level:

- `tests/test_wire_row.cpp` — 10 tests covering `WireRow` saturation,
  closed arithmetic, and `emit_move` byte-correctness. Each one
  shows a representative case (saturate-at-top, saturate-at-bottom,
  clamp-on-construction, etc.); the proof of T1 is the absence of any
  public API that escapes the closure.

- `tests/test_inline_frame.cpp` — 5 happy-path runtime tests plus
  static_asserts proving:
  - All five `InlineFrame<Tag>` specializations are non-copyable.
  - `ShadowWitness` is non-copyable.
  - `FrameBytes` is non-copyable.
  - `ShadowWitness` has no public constructor.
  - `ContentRows` has no public constructor.

  The static_asserts are the *compile-time* proof. They fire if a
  future refactor accidentally adds a copy constructor (e.g. via
  `= default` after removing the move-only deletes), regressing the
  linear discipline.

The pre-existing `tests/test_scrollback.cpp` and
`tests/test_scrollback_vt.cpp` remain unchanged and still pass — they
exercise the legacy `compose_inline_frame` directly and demonstrate
that the new chain coexists with the old API.

## What this design does NOT claim

- It does not prevent bugs *inside* `compose_inline_frame`'s diff
  algorithm. If the per-row emit miscalculates a span, or the wide-char
  snap fails on a new emoji range, the wrong bytes go out for the
  right state. Type-state cannot witness algorithmic correctness;
  property-based fuzzing of the diff is the right complement.

- It does not prevent the kernel or terminal emulator from dropping
  bytes. Out of scope by A1, A2.

- It does not prevent the host process from writing directly to fd 1
  outside the chain. Out of scope by A3.

- It does not prevent silent memory corruption in the
  `verify_shadow` → `compose` window. The probabilistic FNV-1a
  re-check is the defense; this is an A4 hypothesis.

## What this design DOES claim

For any code that compiles against the chain (i.e. uses
`InlineFrame<Tag>` and `compose_inline_frame_v2` rather than the
legacy `InlineFrameState` + `compose_inline_frame` direct path):

- Forgetting to verify the shadow before compose is a compile error.
- Rendering after finalize is a compile error.
- Holding two parallel views of the state is a compile error
  (move-only).
- Advancing state without writing bytes is a compile error (state
  is the return value of `commit_to`).
- Writing bytes without advancing state is a compile error
  (`FrameBytes` is the only producer of bytes; `commit_to` is the
  only consumer).
- Targeting a cursor row outside the viewport is a compile error
  (`WireRow` is the only argument type accepted by `emit_move`).
- Computing row count from the wrong source is a compile error
  (`ContentRows` has one producer).
- Over-committing scrollback to a state that has already
  advanced is a compile error (`ScrollbackMarker` is consumed by
  exactly one `commit` call, which yields a new state).
- Fabricating a trim commit count is a compile error for ledger
  hosts (`ScrollbackDebt` has one producer, `harvest()`, minted only
  from paint-recorded heights).

Each of these used to be a *runtime* failure detected by carefully-
authored checks. Now they fail to typecheck. The compiler is the proof
assistant.

## Runtime migration — done

`Runtime::render` (src/app/app.cpp) dispatches inline frames through
`std::visit` over `InlineCoherence` — the variant of
`InlineFrame<Empty | Fresh | Synced | Stale | HardReset>` — so the
production renderer consumes the chain end-to-end: the only path into
`Synced` is a successful `commit_to` of a witness-verified compose.
The legacy free-function `compose_inline_frame` survives only as the
internal implementation detail the `InlineFrame<Tag>::render`
transitions call (frame_bytes.cpp / inline_frame.cpp); host code has
no direct route to it.

On the host side, agentty's sealed prefix is a `ScrollbackLedger`
(rendered via `Conversation::Config::ledger`), and its trims mint
commits exclusively through `harvest()` — the Layer 6 discharge. The
`agent_session` example demonstrates the same pattern for new hosts.
