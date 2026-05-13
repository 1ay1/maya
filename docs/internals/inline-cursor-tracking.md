# Inline cursor tracking — why `InlineFrameState` carries `bottom_offset`

Engineering record for the "composer rushes to terminal-bottom on
keypress, leaving a duplicate composer above" bug that surfaced in
agentty after a long-stream + scroll-up + scroll-back-down sequence.
Root cause was a desync between maya's mental model of the cursor's
viewport row and the terminal's actual cursor row. Fix: track the
cursor's distance from viewport-bottom in `InlineFrameState` and use
it to scope the row-diff's cursor_up budget.

## What the user saw

Reliable repro:

1. Submit a question that produces a long streaming response (output
   exceeds terminal height; cursor reaches `term_h - 1` and stays
   there as `\r\n`s scroll the buffer).
2. While streaming, scroll the terminal viewport UP to read history.
3. Stay scrolled up until the stream finishes.
4. Scroll back down — observe that the live frame (composer +
   status bar) sits mid-viewport, with empty rows below it.
5. Press any key in the composer.

**Symptom:** the composer + footer "rush" down to fill the empty
space, leaving a duplicate composer at its previous mid-viewport
position. The composer's old row content (placeholder text or
whatever it was) is still visible above the live frame.

The "ghost" was random in practice — same repro sometimes produced
the duplicate, sometimes didn't.

## Why `Cmd::force_redraw()` (69688c5) was the earlier patch

The original fix at 69688c5 wired a `Cmd::force_redraw()` that
collapsed `in_coherence_` to `Divergent`, wiping `InlineFrameState`
on the first user-input event after streaming settled. The next
`compose_inline_frame` saw `prev_rows == 0`, fell through to the
"first-ever render" branch, and emitted a fresh full frame via
`serialize()`. The trailing `\r\n`s in that emission scrolled the
terminal so the cursor ended at `term_h - 1` and the live frame
naturally landed at viewport-bottom. The old live-frame rows scrolled
up into terminal-native scrollback.

That treated the symptom (visual ghost cleared) but left the
underlying mismatch in place — and produced a different kind of
visual jolt ("composer rushes down on first keystroke"). The user's
diagnosis cut through it: *why is maya thinking there are more rows
above the composer than really exist?*

## The actual mechanism

`compose_inline_frame` keeps `prev_cells` as a copy of the last
emitted live frame and tracks `prev_rows = content_rows` from that
emission. Its cursor model is purely relative: after a successful
emit, the cursor is assumed to be at relative row `prev_rows - 1` —
the last row of the live frame — and the next compose positions the
cursor for emission via `cursor_up` from there.

The math for cursor positioning:

```c++
const int prev_on_screen  = std::min(prev_rows, term_h);
const int updatable_start = prev_rows - prev_on_screen;
// ...
const int delta = first_changed - (prev_rows - 1);
if (delta < 0) {
    int up = std::min(-delta, prev_on_screen - 1);
    ansi::write_cursor_up(out, up);
    ...
}
```

The `min(-delta, prev_on_screen - 1)` clamp encodes a key assumption:
the live frame's bottom row is at viewport row `term_h - 1`, so
cursor_up can travel at most `term_h - 1` rows before hitting the
top of the viewport. That assumption holds in the steady state of
streaming — every `\r\n` at `term_h - 1` scrolls, cursor pins at
viewport-bottom — but it *breaks* when something moves the cursor
up without scrolling new content in.

The shrink path is exactly such a thing. When `content_rows <
prev_rows` (e.g. the status_bar shrinks by a row when phase goes
`Streaming → Idle`), the shrink loop emits:

```c++
for (int i = 0; i < extra; ++i) out += "\r\n\x1b[2K";
if (extra > 0) ansi::write_cursor_up(out, extra);
```

If there's room below cursor (cursor not at `term_h - 1` when shrink
starts), the `\r\n`s just move the cursor down without scrolling,
clearing the abandoned rows, and the final `cursor_up(extra)` brings
the cursor back. **Net effect: cursor moves from viewport row `term_h
- 1` to viewport row `term_h - 1 - extra`.**

Maya's invariant ("cursor is at relative row `prev_rows - 1` of the
live frame") still holds because `prev_rows` was simultaneously
updated to `content_rows`. But the *viewport-relative* position has
changed: the cursor is no longer at `term_h - 1`, it's at `term_h -
1 - extra`.

On the next compose (user keystroke), the diff scans
`[updatable_start, common)`. If `first_changed` lands at
`updatable_start = prev_rows - term_h` (the lowest "on-screen" row
in maya's model), the cursor_up request is `(prev_rows - 1) -
(prev_rows - term_h) = term_h - 1`. The clamp at `prev_on_screen -
1 = term_h - 1` doesn't bite.

But the **terminal** only has `term_h - 1 - extra` rows of vertical
travel available before hitting viewport row 0. The terminal
silently clamps cursor_up at row 0. Maya emits as if cursor is at
relative row `first_changed` of the live frame, but it's actually
`extra` rows below that in viewport terms.

The per-row emit then walks `\r\n` per row, advancing cursor down.
Maya emits `term_h` rows starting from where it thinks first_changed
is — at viewport row 0 instead of viewport row `-extra`. After the
emits, cursor lands at viewport row `term_h - 1`. The composer (last
row of frame) ends up at viewport row `term_h - 1` instead of
`term_h - 1 - extra`. The OLD composer at viewport row `term_h - 1
- extra` keeps showing whatever content the new emit *thought* it
was putting there but actually put `extra` rows lower.

That's the visible duplicate.

## Why it was random

`first_changed` only lands in the clamp-affected range when *some
row* in `[prev_rows - term_h, prev_rows - term_h + extra)` happens
to differ between `prev_cells` and the current canvas. Sources of
incidental row-level differences across frames with otherwise stable
model state:

- Tool body previews whose internal rendering depends on time (live
  bash output tail, mid-execution elapsed counter)
- Component cache misses for ephemeral `ComponentElement`s without a
  `cache_id` — re-rendering produces marginally different cells
  (style_id interning timing, attachment ordering)
- Status_bar's sparkline / shortcut_row reading `steady_clock::now()`
  during the render
- Spinner frame rolling over for any active phase indicator

When none of those rows differ, the diff finds `first_changed` at
the composer's row directly, the cursor_up delta is small (within
budget), no clamp, no shift, no ghost. When one of them differs, the
diff finds `first_changed` earlier, the clamp bites, and the ghost
appears.

That's why the bug felt non-deterministic on identical-looking
sessions.

## The fix — `InlineFrameState::bottom_offset`

Track the cursor's distance from terminal-bottom across compose
calls. The field starts at 0 (assume initial cursor at viewport-
bottom, the typical inline-mode handoff state), and updates at the
end of every successful emit:

```c++
state.bottom_offset =
    std::max(0, state.bottom_offset + (prev_rows - content_rows));
```

- `content_rows > prev_rows` (grow) → bottom_offset decreases.
  Capped at 0 because growth that would push cursor below viewport
  bottom scrolls instead.
- `content_rows == prev_rows` (stable) → bottom_offset unchanged.
- `content_rows < prev_rows` (shrink) → bottom_offset increases by
  the shrink amount; the shrink path's `cursor_up(extra)` pulled the
  cursor up by `extra` rows.

The cursor_up budget then uses an `effective_term_h = term_h -
bottom_offset` instead of bare `term_h`:

```c++
const int effective_term_h = std::max(1, term_h - state.bottom_offset);
const int prev_on_screen   = std::min(prev_rows, effective_term_h);
const int updatable_start  = prev_rows - prev_on_screen;
```

Now `updatable_start` excludes the rows that maya thought were
"on-screen but actually unreachable because cursor is above the
viewport-bottom by `bottom_offset`." The diff scan starts at the
first row that's truly reachable from the cursor's actual position;
`first_changed` can't land in the clamp range; cursor_up budget
matches what the terminal will actually deliver; emits land where
maya intends.

## Invariants for future readers

1. After every successful `compose_inline_frame` emit, the cursor is
   at viewport row `term_h - 1 - state.bottom_offset`.
2. `state.bottom_offset` is the only persistent state that captures
   the cursor's vertical position relative to the viewport.
   `state.prev_rows` is *relative* to the live frame, not the
   viewport.
3. The shrink path's `cursor_up(extra)` raises `bottom_offset` by
   `extra` *only when there was room below to absorb the
   intervening `\r\n`s*. If `\r\n` had scrolled instead (no room
   below), `bottom_offset` stays 0 because the cursor never moved
   up — it was always at `term_h - 1`. The update formula
   `max(0, ... + (prev_rows - content_rows))` handles both cases
   uniformly because shrink-with-scrolling and shrink-without-
   scrolling produce the same end-state delta when measured against
   `bottom_offset`'s starting value.
4. `state.reset()` clears `bottom_offset` to 0, matching the
   convention used by `handle_resize` / `Cmd::force_redraw`: after
   a forced re-anchor, the next emit will land the cursor at
   viewport-bottom and `bottom_offset == 0` becomes accurate.

## What this doesn't fix

The user-scroll itself is invisible to the renderer; we still don't
know when the user is scrolled-up. The fix doesn't address scenarios
where the user's scrolling moves the *terminal viewport* relative
to the cursor. That's a separate "the terminal lied to us about
where the cursor is" class — typically resolved by terminal auto-
snap on output, which the user's terminal does only partially.
What the fix DOES address is the cases where maya itself emitted
sequences that moved the cursor up without scrolling, and lost
track of where the cursor ended up.
