# Wire Efficiency — bytes on the wire, and how to shrink them

> Status: **measured baseline + gate landed.** Adaptive coalescing and the
> binary grid frame are designed here and scoped as follow-ups.

## Why this matters

CPU per-frame cost is already at its floor (sub-millisecond, flat with
transcript length — see the render audit). On a **local** tty that is the whole
story. But agentty is routinely driven over **mosh / SSH / Tailscale**, and on a
remote link smoothness is governed not by CPU but by **how many bytes each
frame pushes onto the wire**. A frame that emits 4 KB of re-styled ANSI stutters
on a congested link where a 200-byte frame glides.

Nothing used to measure this. `maya/tests/wire_bytes_bench.cpp` now does, and it
gates in CI (deterministic byte counts — no timing flake).

## The baseline (measured)

Streaming a realistic 5.5 KB assistant answer (prose + bold/code spans + fences +
lists) through the **real** `FrameBuffer` diff-emitter, one diff frame per
appended chunk, at W=100 H=400:

| chunk granularity | B/frame median | B/cell | total wire |
|-------------------|---------------:|-------:|-----------:|
| token (4 B)       | 42             | 6.63   | **70.3 KB** |
| word  (8 B)       | 46             | 4.34   | 44.7 KB    |
| line  (40 B)      | 87             | 2.52   | 20.9 KB    |
| burst (200 B)     | 319            | 2.05   | 28.8 KB    |

Two facts jump out:

1. **Median bytes/frame is already tiny (42 B).** maya's differential SGR, CUP
   elision, ASCII batching, and EL-clear coalescing are doing their job. There
   is no fat to trim in the per-frame *format* for ASCII.

2. **`B/cell` for the token stream is 6.63, and total wire is 70 KB for a 5.5 KB
   document — a 12.8× amplification.** The amplification is *per-frame
   navigation overhead*: each of the 1369 frames pays a fixed CUP + SGR tax to
   position the cursor at the handful of cells it changed. 1369 frames ×
   ~50 B of overhead = the missing ~55 KB.

The overhead scales with **frame count**, not content. So the lever is: **emit
fewer, fuller frames.**

## The ASCII-native win: frame coalescing (measured)

Batch N appended chunks into **one cumulative diff frame**. The content that
reaches the wire is identical (the diff is cumulative — appends only add cells);
only the per-frame overhead is amortised. **This is pure ANSI. It works on every
terminal. No protocol change.**

| coalesce | frames | total wire | vs 1× |
|---------:|-------:|-----------:|------:|
| 1        | 1369   | 70.3 KB    | 1.00× |
| 2        | 685    | 44.7 KB    | 1.57× |
| 4        | 343    | 31.7 KB    | 2.22× |
| 8        | 172    | 22.5 KB    | 3.12× |
| 16       | 86     | 18.2 KB    | 3.86× |
| 32       | 43     | 15.5 KB    | **4.53×** |

At coalesce 32 the per-frame size is still only ~200 B, and total wire asymptotes
toward ~15 KB — the irreducible cumulative diff of the final document. Everything
above that line was navigation overhead.

### How to ship it (follow-up)

The inline render loop (`src/app/inline.cpp::render_live`) **already** defers a
frame when the writer's non-blocking residue won't drain — deferred frames are
naturally coalesced because the model keeps advancing state, so the next
successful compose renders the latest content. That is reactive coalescing at
saturation.

The upgrade is **proactive, backpressure-adaptive coalescing**:

- Measure the wire's actual drain rate from the residue buffer (the data is
  already there: `Writer::has_residue()` / `try_drain_residue()` return the
  parked-byte count).
- Derive a target frame budget: `frames/sec = drain_bytes_per_sec / avg_frame_bytes`.
- Between renders, **accumulate** token deltas; compose one cumulative frame per
  budget tick. On a fast local tty the budget is effectively unbounded (run
  free); on a congested mosh link it drops to exactly the cadence the wire
  absorbs, and intermediate frames coalesce into the next DEC-2026 atomic swap
  — so the user sees fewer, *complete* frames instead of torn partials.

This is adaptive vsync for the terminal, driven by measured backpressure rather
than a fixed tick. **Risk: it touches the inline-frame backpressure state
machine, which has documented scrollback-commit hazards — land it as its own
reviewed change with the wire bench as the proof.**

## The cooperating-host win: binary grid frames

For a host that speaks maya's grid protocol (`RenderBackend::Grid`), ANSI is the
wrong encoding entirely — it re-serialises a cell grid as text. A **binary
damage-run frame** encodes the same frame as:

```
frame  := style_dict_delta  run*
run    := (row:varint, col:varint, len:varint, cell[len])
cell   := (glyph:varint, style_ref:varint)   // style_ref indexes the dict
```

With a **shared style dictionary** (interned once, referenced by index
thereafter), the token stream drops from **6.63 B/cell toward ~1.2** — a further
~5× on top of coalescing, and the cursor-navigation tax disappears (runs carry
their own coordinates). The scrollback-commit protocol that makes this safe
already exists in the grid backend; only the frame *body* format changes.

`B/cell → 1.2` is the north star the bench prints on every run.

## What landed now

- `maya/tests/wire_bytes_bench.cpp` — measures bytes-on-the-wire per streaming
  frame through the real emit path, prints the baseline + coalesce sweep, and
  **gates** two invariants deterministically:
  - coalescing never *increases* total wire (cumulative-diff append-only proof);
  - the coalesce-32 win stays above 2× (guards the per-frame overhead shape).
- Run it: `./build/maya_tests wire_bytes_bench` (add `WIRE_VERBOSE=1` for detail,
  `WIRE_NOGATE=1` to measure without asserting).

## What's next (in priority order)

1. **Adaptive backpressure coalescing** (ASCII, every terminal) — the 3–5× win,
   scoped above. Highest impact.
2. **Binary damage-run grid frame + style dictionary** (cooperating host) — the
   further 5× and `B/cell → ~1.2`.
3. **Speculative tail echo** (mosh-grade) — predict the append-only next frames
   and paint them locally, reconcile against the authoritative frame on arrival.
   Hides the whole round-trip for the dominant streaming case. Most impressive,
   most involved; needs a thin cooperating shim.
