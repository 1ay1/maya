# Inline rendering: autogrow and the ghosting trap

This doc is the engineering record for `render_tree_inline_autogrow`
(commit `fc0a07c`, reverting an earlier broken attempt at the same
idea). It's preserved because the failure mode — ghosting + duplicated
rows when content briefly overflows the terminal — is non-obvious from
reading either the old or the new code in isolation, and the
constraint that distinguishes the working version from the broken one
is one line.

## Context

Inline mode (`src/app/inline.cpp`, the `InlineSynced` branch of
`Runtime::render` in `src/app/app.cpp`) renders into a canvas that
lives in terminal scrollback, not the alternate screen. The composer
(`compose_inline_frame` in `src/render/serialize.cpp`) diffs the new
canvas against the cached previous frame's cell buffer
(`InlineFrameState::prev_cells`) and emits relative cursor moves to
rewrite only the rows that changed.

Content height is data-driven. A streaming agent UI grows the canvas
row by row as tokens arrive. The framework doesn't know up front how
tall a frame will be — the layout engine computes it.

## The legacy pattern (pre-autogrow)

```
canvas.clear();
render_tree(...);                              // build + compute + paint
int ch = content_height(canvas);
if (ch >= canvas.height() && !layout_nodes.empty()) {
    int needed = layout_nodes[0].computed.size.height.raw();
    if (needed > canvas.height()) {
        canvas.resize(w, needed + 8);
        canvas.clear();
        render_tree(...);                       // build + compute + paint
        ch = content_height(canvas);
    }
}
```

The shape: paint first, measure after, resize if too small, paint
again. `render_tree` internally runs `build_layout_tree` →
`layout::compute(w, canvas.height())` → `paint_element`. The first
pass uses `canvas.height()` as the height constraint; if the natural
height exceeds it, the second pass uses the resized height.

This pattern had two structural problems:

1. **OOB cache window.** The `ComponentElement` cell-region cache
   (`renderer.cpp`, the `ComponentElement` branch of `paint_element`)
   captures painted cells *back from the canvas* after rendering a
   component. If the first paint pass overhung the canvas — because
   the canvas was about to be grown but hadn't been yet — the capture
   loop read past `cells_.size()`, populating the cache with garbage
   StylePool IDs. The next frame's fast-path blit copied that garbage
   into the live canvas, and `emit_cell_run` crashed on the bogus
   style ID. Commit `65d946a` band-aided this by clipping the capture
   read to canvas bounds, but the OOB *window* — a paint pass into a
   too-small canvas — was still there.

2. **Wasted paint on overflow.** When content exceeded the initial
   500-row canvas (rare but real for long agent responses), the
   framework did two complete paint passes — discarding the first.

The cleanest fix is to *size the canvas before painting at all*.

## The first attempt (broken — reverted by `e234066`)

```
constexpr int kBigH = 1 << 20;
constexpr int kHSlack = 8;

// Build layout once with an "infinite" height constraint.
build_layout_tree(root, layout_nodes, theme);
layout_nodes[root_idx].style.width = Dimension::fixed(w);
layout::compute(layout_nodes, root_idx, w, kBigH);

const int natural_h = layout_nodes[root_idx].computed.size.height.raw();

// Resize if needed, then paint with that single layout result.
const int target = std::max(min_height, natural_h + kHSlack);
if (canvas.height() < target) canvas.resize(w, target);

paint_element(root, canvas, pool, layout_nodes, root_idx, 0, 0);
```

One compute, one paint. The canvas is sized correctly before paint, so
the OOB window is closed. Both problems above are fixed.

It also caused visible ghosting and row duplication in interactive
sessions.

## What broke

The single line: `layout::compute(..., kBigH)`.

The layout engine takes its `height` parameter as the **available
height** for the root container. That value flows down to children as
their containing block's main-axis size. Elastic children resolve
against it:

- `flex-grow > 0` children in a column container distribute a share
  of the parent's remaining height. Under `kBigH`, "remaining" is
  pathologically large.
- `height: 100%` / `align-items: stretch` cross-axis resolves against
  the parent's available height.
- Min/max clamps fire under tight constraints that don't fire under
  loose ones, and vice versa.

Legacy passed `canvas.height()` (≈ 500 rows initially) as the
constraint. Elastic children distributed sensibly. The painted height
was close to the natural content height because the constraint
*disciplined* the distribution.

The new code passed `kBigH`. Elastic children claimed huge vertical
extents. The painted height routinely exceeded the terminal's visible
height — a 50-row painted frame on a 24-row terminal.

## Why that ghosts

After paint, `InlineFrameState::prev_rows` records the painted height
(say 50). The terminal scrolled the older content out; only the
bottom 24 rows are visible. The next frame calls `compose_inline_frame`,
which positions the cursor relative to where it left off:

```cpp
const int cursor_row_start = prev_rows - 1;           // = 49
const int delta = first_changed - cursor_row_start;   // e.g. -45
if (delta < 0) {
    int up = std::min(-delta, prev_on_screen - 1);    // capped at 23
    if (up > 0) ansi::write_cursor_up(out, up);
    out += '\r';
}
```

`prev_on_screen = min(prev_rows, term_h) = 24`. The `prev_on_screen - 1`
cap exists for a good reason: we must not scroll back into
scrollback-committed history. But it means when we *need* to move up
45 rows to reach `first_changed`, we move only 23. The cursor lands at
the wrong physical line.

The per-row emit loop then proceeds as if the cursor were at
`first_changed`:

```cpp
for (int y = first_changed; y <= last_row_to_visit; ++y) {
    if (y > first_changed) out += "\r\n";
    // emit row y's diff
}
```

Each `\r\n` advances by one line. Because the cursor started 22 lines
*below* where the loop assumes, every row gets written 22 lines too
far down. Old content above stays visible; new content at the bottom
overwrites whatever was previously at those positions.

On the next frame when content shrinks back to a normal size, the
cycle repeats with a different misalignment, producing the duplicated
rows the user observes on the way back to a "normal" view.

The legacy code never tripped this because elastic flex distribution
under `canvas.height()=500` produced painted heights close to the
natural content height — content rarely overflowed the terminal in a
single frame.

## The fix

Keep `compose_inline_frame` untouched. Restore legacy's two-compute
pattern, but skip the wasted first paint:

```cpp
// First compute — same constraint legacy passed; flex grow distributes
// under a realistic budget.
build_layout_tree(root, layout_nodes, theme);
layout_nodes[root_idx].style.width = Dimension::fixed(w);
layout::compute(layout_nodes, root_idx, w, canvas.height());
const int needed_h = layout_nodes[root_idx].computed.size.height.raw();

// Resize if too small, then RECOMPUTE under the new height constraint.
// The second compute is what produces the painted layout — flex
// children distribute identically to legacy's second compute.
const int target = std::max(min_height, needed_h + kHSlack);
if (canvas.height() < target) {
    canvas.resize(w, target);
    layout::compute(layout_nodes, root_idx, w, target);
}

// Single paint pass — into a correctly-sized canvas.
paint_element(root, canvas, pool, layout_nodes, root_idx, 0, 0);
```

Two changes from the broken attempt:

1. **First compute at `canvas.height()`, not `kBigH`.** Flex
   distribution matches legacy. The layout engine still reports
   natural content height in `computed.size.height` even when it
   exceeds the constraint, so we still discover the needed size.
   For frames that fit, this is the only compute.

2. **Recompute after resize.** The painted layout is the result of
   the *post-resize* compute, with the new height as the constraint.
   That's exactly what legacy's second `render_tree` call produced.
   Byte-for-byte visual equivalence with legacy.

## What's preserved

| Property | Legacy | Broken attempt | Working version |
|---|---|---|---|
| Painted layout matches legacy | — | ✗ (kBigH skew) | ✓ |
| OOB cache window closed | ✗ | ✓ | ✓ |
| Paint passes per frame (no overflow) | 1 | 1 | 1 |
| Paint passes per frame (overflow) | 2 | 1 | 1 |
| Compute passes per frame (no overflow) | 1 | 1 | 1 |
| Compute passes per frame (overflow) | 2 | 1 | 2 |

The working version is at least as fast as legacy in every case, and
strictly faster when content overflows (one paint saved). The OOB
window is closed because we never paint into an undersized canvas.

## Invariant for future readers

> The layout that gets painted must come from a `layout::compute` call
> whose height parameter is at least as large as the canvas about to
> receive the paint.

If you ever find yourself wanting to merge the two computes back into
one — by passing `kBigH` to the first compute, or by trusting that the
first compute's elastic distribution is the same as it would be under
a different constraint — re-read this doc. The two computes aren't
redundant.
