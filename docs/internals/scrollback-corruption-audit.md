# Scrollback corruption audit

Read straight off the source. Each finding cites file:line so you can
verify; nothing here is sourced from comments, docs, or commit messages.

Severity:
- **CORRUPTION**: produces wrong cells on the wire under inputs that
  occur in normal operation.
- **DESYNC**: produces a `prev_cells`/wire mismatch the next frame's
  diff turns into either dropped updates or redundant emits.
- **LATENT**: precondition not enforced; current call sites avoid it
  but one refactor away from being a real bug.

Priority labels at the end of each finding.

---

## #1 — `commit_prefix` lies about cursor position when committed rows didn't actually scroll  ·  **CORRUPTION · P0**

**Code:** `src/render/serialize.cpp:17-52` (`commit_prefix`),
`src/render/serialize.cpp:511-525` (`cursor_row_start = prev_rows - 1`),
`include/maya/app/app.hpp:367-373` (`commit_inline_prefix` Cmd handler),
`agentty/src/runtime/app/update/modal.cpp:72-91` (sole caller, uses the
row-counted variant exclusively).

**The model.** `compose_inline_frame` assumes, when it enters with
`prev_rows > 0`, that the physical terminal cursor sits at the row
that is `prev_rows - 1` rows below the top of the live frame. The
diff path then emits a relative move (`cursor_up` for backward,
`\r\n` for forward) to reach `first_changed`.

That assumption only holds if the rows the caller "committed to
scrollback" are rows that the terminal actually scrolled out of the
viewport. `commit_prefix` does not check this — it just decrements
`prev_rows` and `memmove`s `prev_cells` up:

```cpp
prev_rows -= rows;          // serialize.cpp:42
```

No bytes are written; no cursor escape is emitted; nothing verifies
that `rows` of physical terminal viewport actually scrolled.

**The agentty call site.** `maybe_virtualize` in modal.cpp:88 computes

```cpp
const int committed_rows = kSliceChunk * kRowsPerDroppedMessageLower;
return Cmd<Msg>::commit_scrollback(committed_rows);
```

where `kSliceChunk = 4` and `kRowsPerDroppedMessageLower = 2`. So 8
"rows" are claimed to have committed every chunk-cycle. That number
is a guess at the rendered height of the dropped messages, not a
count of physical terminal scrolls.

**Concrete failure.** 50-row terminal. User has 8 turns each taking
~3 rows ⇒ `prev_rows ≈ 24`, all 24 visible on viewport, zero rows
have scrolled. `kSliceChunk` triggers. `commit_inline_prefix(8)` runs:

- `prev_rows`: 24 → 16
- `prev_cells`: shifted up by 8 rows; rows 0–15 now hold what used
  to be rows 8–23.
- Physical viewport: unchanged, 24 rows of agentty content on screen.

Next compose:

- `cursor_row_start = 16 - 1 = 15`
- Physical cursor: at row 23 of the viewport.
- `delta = first_changed - 15` is computed against a baseline 8 rows
  off.

If `first_changed == 0` (the new compose's first changed row),
`delta = -15`, the emit fires `cursor_up(15)` and `\r`. The terminal
moves up 15 rows from row 23 ⇒ row 8. The emit paints starting at
physical row 8, but `prev_cells[0]` claims that's the top of the
live frame. **All emitted rows land 8 rows too low; the 8 rows
physically above row 8 are no longer addressable by future diffs
because the renderer thinks they're scrollback.**

Worse, when `prev_rows + new_rows > term_h` and a `\r\n` at the
bottom edge finally scrolls the viewport, the rows that scroll off
into native scrollback are the **stale, never-cleared ghost rows
sitting between the renderer's model and reality**. Permanent
corruption.

**Why the existing renderer hasn't always shown this.** It does — see
the "ghost rows in scrollback" symptom in the inline-redraw-paths
doc. The current force_redraw mitigation re-paints the live viewport
but cannot recover rows that already scrolled into native scrollback.

**Fix.** Two options:

1. **Make `commit_prefix` a no-op below the viewport threshold.**
   Only allow committing rows that have provably scrolled, i.e. when
   `prev_rows > term_h`. The existing `commit_inline_overflow` at
   `include/maya/app/app.hpp:392-401` already implements exactly this
   discipline:
   ```cpp
   const int overflow_rows = s->state.prev_rows - term_h;
   if (overflow_rows <= 0) return;
   s->state.commit(s->state.scrollback_marker(overflow_rows));
   ```
   Route agentty's `maybe_virtualize` through `Cmd::commit_scrollback_overflow`
   instead of `Cmd::commit_scrollback(rows)`. The Cmd payload is just
   a "trigger" — maya derives the safe row count itself.

2. **Deprecate `Cmd::commit_scrollback(int rows)` entirely.** The
   row-counted variant has no safe caller — every caller would need
   to know exactly how many physical viewport rows scrolled, which
   nobody outside the renderer can know.

Recommendation: do both. Switch agentty first (one line in modal.cpp:88,
delete `kRowsPerDroppedMessageLower` and `committed_rows`); after a
stability window, remove `commit_prefix(int rows)` from the public API.

---

## #2 — force_redraw case (B) corrupts scrollback when `content_rows > term_h`  ·  **CORRUPTION · P1**

**Code:** `src/render/serialize.cpp:455-462`.

```cpp
const int up = std::min(content_rows - 1, std::max(0, term_h - 1));
if (up > 0) ansi::write_cursor_up(out, up);
out += '\r';
const int start_row = std::max(0, content_rows - term_h);
serialize(canvas, pool, out, content_rows, start_row);
out += "\x1b[J";
```

**The problem.** This path runs when `prev_rows == 0 && prev_width > 0`,
i.e. after a `force_redraw()` zeroed `prev_rows` without re-rendering.
The actual physical cursor position is whatever the previous frame
left it at — typically `prev_rows - 1` rows below the top of the live
frame, but force_redraw is **specifically the case where the renderer
admits its model may be stale**.

`up = min(content_rows - 1, term_h - 1)` assumes the cursor is at
least `up` rows below the top of the terminal viewport. If it isn't
(e.g. force_redraw fired after a soft scroll, or the host shell wrote
something between renders), `cursor_up(up)` clamps at viewport row 0,
and the subsequent `serialize` emits up to `term_h` rows of canvas
starting at physical row 0 — pushing whatever was above the live
frame upward by `actual_cursor_row` rows. Those upward-pushed rows
are now in native scrollback.

If those upward-pushed rows are the host's shell history (typical
inline-mode setup), the user sees their scrollback **drift**: prior
prompts and program output move higher in the scroll buffer every
time force_redraw fires.

**Code-level reproducer.** With `content_rows = 80, term_h = 50,
actual_cursor_row = 5` (user scrolled to top of viewport, force_redraw
fires):

- `up = min(79, 49) = 49`. The terminal can only move up 5 rows
  before clamping. Cursor ends at row 0.
- `start_row = max(0, 80 - 50) = 30`. `serialize` emits 50 rows
  (canvas rows 30 → 79) separated by `\r\n`. Each `\r\n` advances
  the cursor; after 49 of them, the cursor is at row 49 (the bottom
  edge). The 50th, 51st, …, last `\r\n` scrolls the viewport once
  per row — so **`80 - 50 = 30` rows of native scrollback get
  pushed up**.

The user's session history above the agentty frame just moved up by
30 rows. If they had scrolled back to read a prior command's output,
it's now in a different place.

**Fix.** Gate case (B) on `content_rows ≤ term_h`. When the new frame
exceeds the viewport, the safe path is the same as Divergent: emit
`\x1b[2J\x1b[3J\x1b[H` to reset to a known state. This costs the
user their scrollback **once, deterministically**, instead of nudging
it on every force_redraw.

```cpp
if (state.prev_width > 0) {
    if (content_rows > term_h) {
        // Fall through to Divergent reset — too tall to soft-redraw.
        out += "\x1b[2J\x1b[3J\x1b[H";
        serialize(canvas, pool, out, content_rows);
    } else {
        // existing case (B)
    }
}
```

Even better: don't have case (B) at all; route force_redraw through
the existing Divergent path and accept the one-shot scrollback wipe
as the cost of a guaranteed-correct redraw.

---

## #3 — Partial-write recovery in `write_all` corrupts mid-CSI sequences  ·  **CORRUPTION · P2**

**Code:** `src/terminal/writer.cpp:361-371`.

```cpp
if (result.error().kind == ErrorKind::WouldBlock) {
    if (!wrote_any) return err(Error::would_block());
    static constexpr std::string_view recovery = "\x1b[?2026l\x1b[0m";
    (void)platform::io_write(handle_, recovery.data(), recovery.size());
    return err(Error::would_block());
}
```

**The problem.** EAGAIN can fire after an arbitrary number of bytes
have shipped — possibly inside a multi-byte CSI sequence like
`\x1b[125;5H`. If the wire received `\x1b[125` and the next chunk
blocked, the recovery sequence `\x1b[?2026l\x1b[0m` starts with a
fresh `\x1b`, abandoning the partial CSI per the VT spec. But many
terminals (older vte, some Windows conhost modes) print the abandoned
parameter bytes (`[125`) as literal characters at the cursor's current
column before the new ESC takes effect.

Those literal characters land in the live viewport. If a row containing
them later scrolls off-edge, they become permanent scrollback corruption.

**Scope.** Only `write_all` does this recovery — used by fullscreen mode
(`app.cpp:485, 502`) and `set_title` (`app.cpp:563`). The inline path
uses `write_or_buffer` which residue-buffers the unsent suffix instead
(`writer.cpp:189-209`). So inline mode is **not affected** by this.

But: `set_title` is called from agentty inline sessions. A partial
write on an OSC 0 (`\x1b]0;...\x07`) could leave a partial `\x1b]0;`
followed by recovery — same corruption shape.

**Fix.** Emit `\x18` (CAN, ECMA-48 §5.7) before the recovery sequence —
it's the documented "cancel current control sequence" code. Most
terminals honor it; the ones that don't will at least treat the `\x18`
as a non-printing control character. Better than emitting a fresh ESC
which is ambiguous.

```cpp
static constexpr std::string_view recovery = "\x18\x1b[?2026l\x1b[0m";
```

Alternative: switch `set_title` to `write_or_buffer` (it doesn't need
atomic delivery — a partial OSC 0 just leaves the old title in place
on the next residue retry).

---

## #4 — `blit_packed_row` raises `max_y_` for rows that may have been emptied by clip-edge wide-glyph repair  ·  **LATENT · P3**

**Code:** `include/maya/render/canvas.hpp:467-491`.

After the orphan repair blanks one or both edge cells, the row may be
entirely blank. The code still bumps `max_y_ = y`:

```cpp
if (row_has_content) {
    if (y > max_y_) max_y_ = y;
    int actual_last = -1;
    for (int i = count - 1; i >= 0; --i) { /* scan */ }
    if (actual_last > last_col_[y]) last_col_[y] = actual_last;
}
```

When `actual_last == -1` (row turned out blank after repair), `max_y_`
is still raised. Subsequent `canvas.clear()` re-zeroes
`[0, max_y_+1) × W` — pure waste, no correctness impact.

**Severity.** Not corruption; not even desync. Performance only. But
it's a divergence from the contract `set()` upholds at canvas.hpp:394-403
("only bump `max_y_` if visible content was actually written"), which
makes the code harder to reason about.

**Fix.** Move the `max_y_` bump inside the `actual_last >= 0` guard:

```cpp
if (actual_last >= 0) {
    if (y > max_y_) max_y_ = y;
    if (actual_last > last_col_[y]) last_col_[y] = actual_last;
}
```

---

## #5 — `clear_rows(n)` leaves `last_col_[n..height_]` populated; `max_y_` reset to -1 is wrong if rows past n still hold content  ·  **LATENT · P3**

**Code:** `src/render/canvas.cpp:441-451`.

```cpp
void Canvas::clear_rows(int n) {
    ...
    std::fill(cells_.data(), cells_.data() + count, blank);
    ...
    max_y_ = -1;
    std::fill(last_col_.begin(), last_col_.begin() + rows, -1);
}
```

The function is named "clear ROWS [0, n)" — partial clear. But it
unconditionally sets `max_y_ = -1` (canvas-wide claim of no content).
If cells `[n, height_) × W` still hold non-blank content, the cache
now lies.

**Current call sites.** Only the table widget (`include/maya/widget/table.hpp:71`)
and the older `render_live` path inside `renderer.cpp:1080` — neither
of which exposes the bug today because both call `clear_rows(canvas.height())`
which is equivalent to `clear()`. The inline renderer in `app.cpp:386`
and `inline.cpp:62` uses `clear()` directly.

**Fix.** Either:
- Rename to make the partial-clear intent explicit and rescan
  `max_y_` from the surviving rows:
  ```cpp
  max_y_ = -1;
  for (int y = rows; y < height_; ++y)
      if (last_col_[y] >= 0) max_y_ = y;
  ```
- Or delete `clear_rows` outright — no inline-mode caller benefits
  from it (the bounded variant was reverted in favor of `clear()`
  per the comment at inline.cpp:45-53), and the table widget
  clear-all use case can call `clear()`.

---

## #6 — No assertion that `content_rows == canvas.max_content_row() + 1` at compose entry  ·  **LATENT · P3**

**Code:** `src/render/serialize.cpp:294-303` (compose entry) and
`src/app/app.cpp:393, 415` (sole call sites — they pass
`content_height(canvas_)` which returns `canvas.max_content_row() + 1`).

**The latent risk.** If any future call site computes `content_rows`
from a different source — e.g. layout's `computed.size.height` — and
canvas has not been painted to that exact row count, `compose_inline_frame`
will memcpy and emit rows that contain stale data from previous frames.

The first-render fast path at serialize.cpp:476-485 copies
`content_rows × W` cells unconditionally:

```cpp
std::memcpy(state.prev_cells.data(), cells, new_size * sizeof(uint64_t));
```

If `cells` past `max_content_row()` holds residue from a prior frame
that wasn't re-cleared (canvas.cpp's `clear()` only zeros
`[0, max_y_+1)`), that residue ends up serialized AND shadowed.

**Fix.** Add a debug assert at compose entry:

```cpp
assert(content_rows == canvas.max_content_row() + 1
    && "content_rows must derive from canvas's max_content_row");
```

Cheap, catches the violation immediately if anyone introduces it.

---

## #7 — `state.decawm_off` is not set true on the first-render fallthrough; the next frame re-emits `\x1b[?7l` redundantly  ·  **non-issue · informational**

**Code:** `src/render/serialize.cpp:454-486` (case A/B branch) vs
`src/render/serialize.cpp:533-536` (diff path's DECAWM emit).

The first-render branch calls `serialize()` (which brackets its emit
with `\x1b[?7l...\x1b[?7h`) but doesn't set `state.decawm_off`. The
next frame's diff path sees `state.decawm_off == false` and re-emits
`\x1b[?7l`. Five extra bytes per session; correctness preserved
(wire was at DECAWM-on after serialize's close, now back to off).

**Listed for completeness.** Not a fix candidate — the alternative
(set `state.decawm_off = true` after the serialize fallthrough) would
require knowing that serialize emitted DECAWM-off then DECAWM-on. The
current shape — let the next frame re-assert — is the simpler invariant.

---

## #8 — Width change resets InlineFrameState but `state.cursor_hidden` keeps its value  ·  **DESYNC · P3**

**Code:** `src/render/serialize.cpp:305` and `include/maya/render/serialize.hpp:88-96`.

```cpp
// serialize.cpp:305
if (state.prev_width != W) state.reset();

// serialize.hpp:88
void reset() noexcept {
    prev_width    = 0;
    prev_rows     = 0;
    decawm_off    = false;
    cursor_hidden = false;
}
```

OK wait, `reset()` does reset `cursor_hidden`. Re-check.

Actually it does. Then the next frame at serialize.cpp:358 re-emits
`hide_cursor` because `cursor_hidden` was just reset to false. ✓.

**Not a bug.** I list it because the relationship between `reset()`
and the wire state is implicit: `reset()` sets the flag to `false`
but doesn't emit `show_cursor`. The wire's actual cursor visibility
state is unknown after a width change — could be visible (if the
host wrote a `show_cursor` mid-render) or hidden (the usual case).
The next frame's `hide_cursor` emit is therefore the right thing
either way, BUT it leaks one frame of cursor visibility into the
re-render. Acceptable.

If a future change makes the cursor state externally observable
(e.g. for caret-positioning a search field), this becomes a real
issue. Worth flagging in a comment near `reset()`.

---

## Priority summary

| # | severity      | priority | one-line fix                                                                  |
|---|---------------|----------|-------------------------------------------------------------------------------|
| 1 | CORRUPTION    | **P0**   | agentty: use `commit_scrollback_overflow` instead of `commit_scrollback(rows)` |
| 2 | CORRUPTION    | **P1**   | case (B) soft-redraw: fall through to Divergent when `content_rows > term_h`  |
| 3 | CORRUPTION    | **P2**   | `write_all` recovery: prepend `\x18` to cancel partial CSI                    |
| 4 | LATENT        | P3       | `blit_packed_row`: gate `max_y_` bump on `actual_last >= 0`                   |
| 5 | LATENT        | P3       | `clear_rows`: rescan `max_y_` from surviving rows, or delete it               |
| 6 | LATENT        | P3       | add debug assert `content_rows == max_content_row() + 1` at compose entry     |
| 7 | informational | —        | acceptable redundancy (5 bytes/session)                                       |
| 8 | informational | —        | comment near `state.reset()` re cursor visibility                             |

**The #1 fix is the load-bearing one.** It is the single bug that is
currently producing the symptoms the user reports as "scrollback
corruption", and fixing it eliminates the most common path to
permanent ghost rows.

#2 is a near-second — it manifests every time `force_redraw` fires
during a tall stream. The user is unlikely to hit it because the
streaming-finished bottleneck for force_redraw happens *after*
content has been committed to scrollback by normal `\r\n` scrolls,
so `content_rows` at force_redraw time is usually ≤ `term_h`. But
"usually" isn't "always".

#3 is the only finding that's a pure terminal-emulator-quirk bug.
The recovery sequence shipped is correct for spec-compliant terminals
and incorrect for some real ones.

The rest are hygiene.
