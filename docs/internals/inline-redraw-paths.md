# Inline-mode redraw paths — three cases, three behaviors

Engineering record for the render-correctness work that fixed the
"composer rushes to terminal-bottom on first keypress after a long
stream" symptom, the related scrollback-wipe at startup, and the
interactions between them. Final commits: `aa35eba` (maya) +
`8bf8ac9` (moha). This doc covers what was actually wrong, two
failed attempts, and the design that eventually worked.

## What the user saw

Three distinct bug surfaces, all rooted in the inline renderer's
first-ever-render path:

1. **Composer rushes to bottom.** Reproducer: start a streaming
   response that overflows the terminal, scroll UP during streaming
   and stay there, wait for stream to finish, scroll back DOWN
   (composer + status_bar visible mid-viewport with blank rows
   below), press any key. The composer + footer suddenly jump down
   to the terminal's last rows, leaving a duplicate of their old
   position visible above them.
2. **Scrollback wiped at startup.** Reproducer: run agentty from a
   shell with visible history. Maya's first render erased the entire
   terminal scrollback, destroying the user's session history.
3. **Live frame at top instead of inline.** Reproducer: same as
   (2) but the perceptible symptom — agentty started by painting at
   viewport row 0 (top), not at the cursor's current position. This
   broke the inline-mode convention where TUIs append below host
   content.

(2) and (3) were the same root cause as (1) — all three live in the
diff path's "first-ever render" branch, fired by different triggers
that we initially conflated.

## The Cmd::force_redraw lineage (and why it was the wrong fix)

The original commit `69688c5` added `Cmd::force_redraw()` to clear
the post-streaming "ghost composer" symptom. The bug was: during a
long streaming session, transient composer/footer cells got
committed to terminal scrollback as the live frame scrolled past
`term_h - 1`; on the next user input, those stale rows were visible
on screen but maya's `prev_cells` no longer matched them, so the
diff path emitted bytes that landed at wrong positions, leaving a
visible ghost.

`Cmd::force_redraw()` collapsed `in_coherence_` to `Divergent`. The
next render hit the Divergent path, which emitted
`\x1b[2J\x1b[3J\x1b[H` (clear viewport + **clear scrollback** +
home cursor) and then re-painted the entire frame fresh from row 0
via `serialize()`. That fixed the ghost — but at the cost of:

- Wiping the user's scrollback every time force_redraw fired.
- Producing the "composer rushes to terminal-bottom" jolt because
  the fresh serialize from the home position scrolled the frame
  down to its viewport-bottom-anchored position.

The user's correct intuition: force_redraw was a sledgehammer fix
for a symptom that should be handled gently. The diff path's state
needed refreshing, but the screen didn't need wiping.

## Failed attempt #1: closed-form `bottom_offset`

First fix attempt: track the cursor's distance from terminal-bottom
in `InlineFrameState::bottom_offset`. The formula was

```cpp
state.bottom_offset =
    std::max(0, state.bottom_offset + (prev_rows - content_rows));
```

— shrink raises it (cursor moved up), grow lowers it (cursor moved
down, clamped at 0 when scrolling). `compose_inline_frame`'s
`prev_on_screen = min(prev_rows, term_h - bottom_offset)` used this
to scope the cursor_up budget correctly.

**Worked for the original bug. Broke other things.** The formula
assumed every cursor movement happened cleanly, but the diff path
has edge cases where cursor moves differ from row-count deltas:

- `cursor_up` clamps at viewport row 0 when it would overshoot.
- `\r\n` at viewport row `term_h - 1` scrolls instead of advancing.
- The per-row loop emits **zero** rows when `first_changed >
  last_row_to_visit` (large shrink with no on-screen diff).
- The shrink loop's `cursor_up(extra)` clamps if `extra` exceeds
  the available room.

Each of these makes the actual cursor end up at a different
viewport row than the formula predicts. Across many turns,
`bottom_offset` drifted; eventually `term_h - bottom_offset` shrank
toward 1, the diff path emitted only the bottom row each frame,
and the shrink path's `\r\n` scrolling committed chrome rows to
terminal-native scrollback on every compose. The visible symptom
was a parade of stacked composer + status bar copies in scrollback.

Reverted in `edffdc7`.

## Failed attempt #2: simulate the cursor

Second attempt: model `cursor_viewport_row` precisely by walking
each emitted sequence under the same clamping rules the terminal
applies:

```cpp
// cursor_up(up):    K = max(0, K - up)
// \r\n:             K = min(term_h - 1, K + 1)   // scrolls at bottom
// cursor_down(n):   K = min(term_h - 1, K + n)
```

In principle this is exact — every cursor-affecting emit gets
mirrored in K, so K can't drift from reality. In practice the bug
was elsewhere: when streaming chunks arrived after some idle time,
the chrome disappeared and the new stream content emitted at
viewport row 0 with blank rows below. Likely cause: the K-tracking
correctly captured cursor position but the `prev_on_screen = K + 1`
scoping interacted badly with `commit_prefix` (which shifts
`prev_cells` without updating K) or with some other reset path I
hadn't traced. Didn't fully diagnose before reverting.

Reverted in `f620030`. Lesson: cursor tracking in the diff path
has too many subtle interactions to retrofit cleanly. The next
attempt avoided modifying the diff path entirely.

## The fix that worked

Instead of tracking the cursor's viewport position, **route
force_redraw through a different code path than resize**. Three
triggers, three behaviors, differentiated by one already-existing
piece of state: `InlineFrameState::prev_width`.

### The state-based differentiation

```
prev_width == 0, prev_rows == 0     →  case (A): startup / Divergent reset
prev_width >  0, prev_rows == 0     →  case (B): force_redraw soft redraw
prev_width >  0, prev_rows >  0     →  normal diff path
```

`prev_width` is set the first time `compose_inline_frame` emits
content. It only resets to 0 on `state.reset()`. So a fresh state
(no emit ever happened) is distinguishable from a previously-
emitted state with `prev_rows` zeroed by force_redraw.

### Case (A): startup / Divergent reset

`prev_width == 0` (the truly-fresh state).

`compose_inline_frame`'s first-ever-render branch calls
`serialize(canvas, pool, out, content_rows)` from the cursor's
current position. Each row separator (`\r\n`) advances the cursor;
if the cursor reaches `term_h - 1`, subsequent `\r\n`s scroll. The
live frame appears AT the cursor's starting position and grows
downward (the inline-mode convention).

For **startup**, the cursor is wherever the host shell left it
(typically `term_h - 1` after a prompt). The live frame appears at
the bottom of the viewport; host content above remains visible;
scrollback is preserved untouched.

For **Divergent reset** caused by resize or write-failure recovery,
the Divergent path emits `\x1b[2J\x1b[3J\x1b[H` first — clear
viewport + clear scrollback + home cursor. The cursor is then at
row 0. Serialize emits from there; the frame fills the top of the
viewport. Scrollback is wiped (acceptable per design decision —
resize layouts are sufficiently different that preserving the
previous-width content would just look broken).

Startup is **not** routed through Divergent. `Runtime::create`
pre-seeds `in_coherence_` to `coherent::InlineSynced{}` with a
fresh `InlineFrameState`. The first render hits the Synced visit;
`compose` sees `prev_width == 0` and falls through to (A) — but
without the Divergent path's `\x1b[2J\x1b[3J\x1b[H` ever being
emitted. That's how startup keeps scrollback intact.

### Case (B): force_redraw soft redraw

`prev_width > 0` AND `prev_rows == 0`.

`Runtime::force_redraw()` zeroes `prev_rows` on the existing
`InlineSynced` state but **leaves `prev_width` alone**. The next
`compose` sees `prev_rows == 0` (so the first-ever-render branch
fires) but `prev_width > 0` (so it knows a previous frame is on
the wire). It does a SOFT redraw:

1. `cursor_up(min(content_rows - 1, term_h - 1))` — move up to
   where the new frame's top should land. The terminal clamps at
   viewport row 0 if the request exceeds available distance.
2. `\r` — column 0.
3. `serialize(canvas, pool, out, content_rows)` — emit content_rows
   rows. Each `\r\n` advances the cursor; if the cursor reaches
   `term_h - 1`, the remaining `\r\n`s scroll exactly the rows
   that need to overflow into native scrollback.
4. `\x1b[J` — erase from cursor to end of screen, clearing any
   blank rows below the new frame's bottom (e.g. rows left blank
   by a previous shrink).

**For the user's bug scenario**: cursor was at viewport row K mid-
screen after a stream-finish shrink, content_rows fits above the
cursor (`content_rows ≤ K + 1`). cursor_up lands at
`K - content_rows + 1`. serialize emits without scrolling. Cursor
returns to row K. The composer is back at the same viewport row
it was at before the redraw — no rush, no ghost. Scrollback fully
preserved.

**For the overflow case** (`content_rows > K + 1`, e.g. mid-stream
force_redraw with a huge frame): cursor_up clamps at row 0,
serialize emits `content_rows` rows, the overflow scrolls into
native scrollback exactly as a normal stream would. Same behavior
as the old force_redraw for that case.

### Scope contract: case (B) is viewport-only

Case (B) is **deliberately bounded to the live viewport**. The
emit caps at the last `term_h` rows of the canvas
(`start_row = max(0, content_rows - term_h)`); any rows of the
inline frame that have already overflowed into native scrollback
are not re-emitted, and in inline mode **cannot** be re-emitted.

This is a hard property of the medium, not a TODO. Inline mode
shares the terminal viewport with the host's pre-existing
scrollback — the shell prompts and program output that lived
above the composer before maya started. Any escape capable of
overwriting committed scrollback rows would also overwrite the
host's content, because at the VT level there is no distinction
between "my scrollback" and "the host's scrollback" once a row
has scrolled off the top edge. The cells are owned by the
terminal emulator and are immutable to the application.

**What case (B) fixes:**

- Ghost cells inside the live viewport — composer outline
  survivors of a stream-finish shrink, stale status / footer
  rows below the new `content_rows`, SGR residue from a
  half-written frame.
- `prev_cells` / wire desync — caller (`Runtime::force_redraw`)
  zeroed `prev_rows` to mark "diff state is stale, redraw
  fresh", so the per-row diff skips its usual byte-identical
  fast paths and re-emits every visible row.

**What case (B) does NOT fix:**

- Scrollback corruption (a stray subprocess wrote directly to
  fd 1 mid-frame; a tmux pane swap mangled the rows above the
  viewport; a terminal emulator dropped bytes during a resize).
  Those rows are off-viewport, committed, and unreachable.

The user-facing recoveries for scrollback corruption are:

1. **The terminal emulator's own redraw** — most emulators bind
   their own Ctrl-L (or an equivalent menu item) to a full
   repaint of the emulator's local cell grid, which IS able to
   reach scrollback rows because the emulator owns them.
2. **A resize event** — maya treats a resize as a
   coherence-collapse, routing the next render through the
   Divergent path. Divergent emits `\x1b[2J\x1b[3J\x1b[H` (wipe
   viewport + wipe scrollback + home cursor) before painting
   fresh from case (A). The scrollback wipe is destructive to
   the host's prior output, which is why it is deliberately
   gated on a resize event and never bound to a keystroke.

**Implications for app authors wiring a user-facing "redraw"
hotkey** (e.g. Ctrl-L → `Cmd::force_redraw`): mirror this scope
in your user-facing docs. Users coming from full-screen apps
(vim, htop, less) expect Ctrl-L to repair anything, because
those apps own the entire alternate screen buffer and can
repaint every visible cell. An inline-mode app shares the
viewport with whatever was there first, and its redraw
affordance must reflect that.

### The Divergent path still does the aggressive clear

Resize and write-failure recovery still go through the Divergent
path, which still emits `\x1b[2J\x1b[3J\x1b[H` and then runs the
fresh paint via case (A). This was a deliberate call by the user
("resize can wipe the scrollback") — a resize means the layout
constraints just changed and any previous frame's positioning is
now wrong, so a full reset is the appropriate response. The
scrollback wipe (`\x1b[3J`) is a side effect, accepted in the
resize case because the previous-width content in scrollback
would have been rendered at the wrong column count anyway.

## Why this works where the cursor-tracking attempts didn't

The cursor-tracking approaches tried to retrofit a piece of
runtime state (where the cursor actually is) into the diff path's
existing math. That math has too many edge cases (the per-row
loop's zero-iteration case, the shrink path's scrolling behavior,
the cursor_up clamp at row 0, commit_prefix's row-count shift
without cursor adjustment) for the retrofit to be correct without
also rewriting the diff path itself.

The state-based differentiation sidesteps the problem entirely:

- The normal diff path is unchanged (no cursor tracking, no
  modified prev_on_screen math).
- The force_redraw case (B) uses cursor_up + serialize + \x1b[J
  — a sequence that's CORRECT BY CONSTRUCTION regardless of where
  the cursor is, as long as the terminal's natural clamping
  behavior is what we want. If `content_rows - 1` is more than
  the cursor's distance from row 0, the cursor clamps — we don't
  need to know how far it actually moved, because the next
  `serialize` emits content_rows rows from wherever the cursor
  landed and \x1b[J clears below either way.
- Startup case (A) is the unchanged old behavior, just no longer
  routed through Divergent.

No drift, no edge cases, no retrofit. Each case has well-defined
input state and a code path that handles it correctly.

## Invariants for future readers

1. `state.prev_width == 0` ⟺ no successful frame has ever been
   emitted from this state. Set on the first successful emit;
   reset only by `state.reset()`.
2. `state.prev_rows == 0` AND `state.prev_width > 0` ⟺
   `force_redraw` was called between this compose and the last
   successful emit.
3. The Divergent → Synced transition uses a fresh
   `InlineFrameState` (prev_width = 0), routing it to case (A).
4. The default `Runtime` state for inline mode is
   `InlineSynced{InlineFrameState{}}`, NOT `Divergent`. Setting it
   to Divergent (e.g. for fullscreen mode) is the explicit signal
   that the aggressive scrollback-wiping clear should happen.

## Choosing a recovery primitive

Three host-callable primitives interact with the inline render
state. They are NOT interchangeable; using the wrong one is the
class of bug that produces "corruption after thread switch" or
"scrollback wiped on Ctrl-L."

### `Cmd::commit_scrollback_overflow` — bookkeeping only

**What it does.** Calls `Runtime::commit_inline_overflow()`,
which advances `prev_cells` by `max(0, prev_rows - term_h)`
rows and recomputes the shadow hash. **Zero bytes written to the
wire.**

**After.** `prev_rows ≤ term_h`, `updatable_start == 0`. The
next compose's diff scans every visible row.

**Safety.** The rows dropped from `prev_cells` were emitted to
the wire by earlier streaming `\r\n`s and the terminal already
committed them to native scrollback. We're just acknowledging
that fact — there is nothing on the wire to repair.

**Use when.**
  - Wholesale model swap (thread switch, new thread, checkpoint
    restore). After the swap the new canvas content at rows
    `[0, updatable_start)` differs from `prev_cells`, but the
    diff would skip those rows thinking they're scrollback. The
    commit drops the stale prefix so the diff sees the full
    visible range as the diff window.
  - Bounded-frozen trim (the `agent_session` pattern of dropping
    the oldest N entries from `m.frozen` when it grows past a
    soft cap). The same shadow / wire mismatch arises.

### `Cmd::force_redraw` — soft viewport repaint

**What it does.** Demotes `InlineFrame<Synced>` → `Stale`. The
next compose enters case (B): cursor walks up by
`wire_cursor_rows`, every visible row is re-emitted in place,
then `\x1b[J` erases below.

**Hazard.** Case (B)'s `scroll_n > 0` branch (new frame taller
than the old cursor's offset from viewport top) emits `\n` at
the viewport bottom to make room — each `\n` permanently
scrolls one row of host content into terminal-owned scrollback.
For wholesale model swap this destroys host shell history
above the agentty region. Use
`commit_scrollback_overflow` instead in that case.

**Use when.**
  - Ghost cells visible **inside** the live viewport: composer
    outline survivors of a stream-finish shrink, stale status /
    footer rows below the new content_rows, SGR residue from a
    half-written frame.
  - User-facing "redraw screen" hotkey (Ctrl-L).

### Hard reset (resize-internal) — not host-callable

**What it does.** `Runtime::handle_resize` demotes the inline
state to `HardReset`, whose render emits
`\x1b[2J\x1b[3J\x1b[H` — wipes the viewport AND the terminal's
native scrollback, then repaints fresh via case (A).

**Why not host-callable.** Wiping native scrollback destroys
pre-agentty shell history. SIGWINCH gets away with it because
the terminal emulator is already repainting that region itself,
and the layout invalidation makes the previous frame's
positioning meaningless anyway. No other situation justifies
the destruction.

### Chooser table

| Situation                                       | Right primitive                |
|-------------------------------------------------|--------------------------------|
| Ghost cells inside the live viewport            | `Cmd::force_redraw`            |
| Ctrl-L style user redraw hotkey                 | `Cmd::force_redraw`            |
| Wholesale model swap (thread switch / restore) | `Cmd::commit_scrollback_overflow` |
| Bounded-frozen trim (drop oldest N rows)        | `Cmd::commit_scrollback_overflow` |
| Terminal genuinely corrupted (external write)   | (none — resize only)           |

### Common mistake: `force_redraw` on model swap

The instinct on a wholesale model swap is "my prev_cells
shadow no longer matches what the user should see, force a
full repaint." That instinct is correct; the choice of
primitive is not.

`force_redraw`'s case (B) was designed for **in-viewport
ghosts**, where the new frame is the same height or shorter
than the prior painted region. The `scroll_n > 0` branch
(handling the new-frame-taller case) emits bottom-edge `\n`s
that scroll viewport content into native scrollback. Under
streaming, this is correct: the rows scrolling off are the
frozen / committed prefix of the current conversation. Under a
model swap, the rows scrolling off are unrelated host content
(or the previous thread's tail), and the scroll permanently
destroys them.

`commit_scrollback_overflow` is the right primitive: it tells
maya "the overflow rows in prev_cells are gone" without emitting
any bytes, then the normal diff path repaints the full viewport
against the new canvas correctly. Mid-viewport seams (where the
diff was skipping rows it thought were committed scrollback) go
away; host scrollback above is preserved.

See `agentty/src/runtime/app/update/picker.cpp` —
`ThreadListSelect` / `NewThread` handlers — for the canonical
application of this rule, with the commit-revert-recommit history
in the block comment.

## File map

- `maya/include/maya/render/serialize.hpp` — `InlineFrameState`
  declaration (the state struct).
- `maya/src/render/serialize.cpp` — `compose_inline_frame`'s
  first-ever-render branch with the (A)/(B) differentiation.
- `maya/include/maya/app/app.hpp` — `Runtime::force_redraw()` that
  zeroes `prev_rows` on the existing `InlineSynced` state instead
  of collapsing to Divergent.
- `maya/src/app/app.cpp` — `Runtime::create()` pre-seeds
  `in_coherence_` to `InlineSynced{}` for inline mode; the
  Divergent path's `\x1b[2J\x1b[3J\x1b[H` pre-clear is kept for
  resize / write-fail.
- `agentty/src/runtime/app/update/stream.cpp` — `StreamFinished`
  handler arms `m.ui.needs_force_redraw = true` so the next user
  input fires `Cmd::force_redraw()`.
- `agentty/src/runtime/app/update.cpp` — consumes
  `needs_force_redraw` on user-input messages, batches
  `Cmd::force_redraw()` alongside the regular Cmd.
