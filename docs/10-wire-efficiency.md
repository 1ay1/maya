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

The upgrade is **proactive, backpressure-adaptive coalescing** — **SHIPPED**
(see `include/maya/app/wire_coalesce.hpp` + the gate in `Runtime::render()`):

- Every inline `render()` samples `writer_->has_residue()` on entry and folds
  it into an EWMA congestion estimate in `[0,1]` (α=0.25 → ~4-frame memory).
- That estimate maps to a **minimum compose interval**: 0 ms below a 0.15
  floor (fast/local wire → the gate is inert), ramping linearly to a 33 ms
  (≈30 fps) cap when the wire saturates.
- A `render()` that arrives inside the interval returns `ok()` WITHOUT
  composing — the frame is coalesced. The model keeps advancing between the
  caller's RAF re-fires (≤16 ms during a stream), so the next compose is a
  single **cumulative** diff covering every append that landed meanwhile,
  removing the per-frame CUP+SGR tax.

Measured on a simulated saturated wire at 60 fps: **~49% of frames coalesce**
(composes drop 120→61), each remaining frame cumulative — the mechanism behind
the bench's 3–5× wire reduction. Composes never hit zero (no starvation).

Safety (why this was low-risk despite touching the render path):

- The gate lives at the TOP of the inline path, before the Witness Chain
  dispatch — it never touches chain state; a coalesced frame is a clean early
  `ok()`, identical to the existing WouldBlock defer.
- It only ever DELAYS a compose, and only while congestion is already high
  enough that the wire couldn't have shown the intermediate frames anyway. On
  a fast wire congestion stays ~0 and the gate is a no-op.
- The delay is bounded (≤33 ms) and the caller's streaming RAF guarantees a
  re-fire, so a coalesced frame is never stranded.
- `MAYA_NO_COALESCE=1` disables it entirely.
- Verified: 514/514 maya (Witness Chain + scrollback intact), 321/321 agentty
  incl. all 26 scrollback/reveal/inline/stream tests; the decision math has 6
  dedicated unit tests (`tests/test_wire_coalesce.cpp`).

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
thereafter), the per-frame style table stops re-sending unchanged style
definitions. The scrollback-commit protocol that makes this safe already exists
in the grid backend.

### Measured reality (this overturned the roadmap's guess)

The wire bench now drives the **real grid emit path** side-by-side with ANSI
(`stream_grid`). The result was the opposite of the assumption above:

| scenario | ANSI | grid v2 | grid + dictionary |
|----------|-----:|--------:|------------------:|
| token (4 B) | 70.3 KB | 163.9 KB | 146.5 KB |
| word  (8 B) | 44.7 KB | 95.7 KB  | 86.4 KB  |
| line  (40 B)| 20.9 KB | 38.8 KB  | 36.2 KB  |

**The grid backend is currently ~2× WORSE than ANSI on the wire, not better.**
ANSI's diff emits only the actually-changed *cells* with differential SGR
(often 0 style bytes); the grid frame emits an 8-byte fixed run header
(`row+col+len+style`, u16×4) per run plus a per-frame style table. On streaming
content the run-header tax dominates.

**The style dictionary — SHIPPED (opt-in, byte-exact) — closes 5–12% of that
gap** (`grid+dictionary` column), but it was never the dominant cost. It is a
real, safe win where grid is *already* in use, and it is the correct primitive
to have; it just isn't the thing that makes grid beat ANSI.

**The real lever, now that it's measured, is the run header, not the style
table:** varint-encode `row/col/len/style` (each fits in 1 byte for typical
content) to cut the 8-byte header to ~4, and split runs to skip unchanged
interior cell spans (not just the leading columns). That is the next grid
protocol revision — targeted at the measured bottleneck rather than a guess.

### The style dictionary, as shipped

`include/maya/render/grid_emit.hpp` adds an opt-in `StyleAckSet`:

- Pass it to `emit_diff` / `emit_full` and a style's *definition* is sent only
  the first frame the host sees it; later frames reference it by id and omit
  the definition (flags **bit4 `kFlagPartialStyleTable`** tells the host to
  resolve absent ids from its cache).
- A `Full` frame **resets** the ack set (a hard re-state invalidates the host's
  style cache) and re-sends a complete table.
- **Strictly opt-in:** with `ack == nullptr` (the default) the encoder is
  **byte-identical to v2** — no deployed host is affected. A host advertises
  support out-of-band before the caller starts passing an ack set.
- Byte-exactness is pinned by a reference-decoder round-trip test
  (`tests/test_grid_emit.cpp`): a simulated host-side style cache resolves
  every run through several frames incl. a Full-frame reset, and the wire is
  proven strictly smaller on a repeated style.

## What landed now

- `maya/tests/wire_bytes_bench.cpp` — measures bytes-on-the-wire per streaming
  frame through the real emit path, prints the baseline + coalesce sweep, and
  **gates** two invariants deterministically:
  - coalescing never *increases* total wire (cumulative-diff append-only proof);
  - the coalesce-32 win stays above 2× (guards the per-frame overhead shape).
- Run it: `./build/maya_tests wire_bytes_bench` (add `WIRE_VERBOSE=1` for detail,
  `WIRE_NOGATE=1` to measure without asserting).

## What's next (in priority order)

1. ~~**Adaptive backpressure coalescing** (ASCII, every terminal)~~ — **SHIPPED.**
   The 3–5× wire win; see above.
2. **Grid run-header compaction** (cooperating host) — the MEASURED grid win:
   varint `row/col/len/style` (8 B header → ~4 B) + interior-span run splitting.
   The style dictionary (SHIPPED, 5–12%) was the safe first step; this is the
   change that makes grid actually beat ANSI. Needs a host protocol bump.
3. **Speculative tail echo** (mosh-grade) — predict the append-only next frames
   and paint them locally, reconcile against the authoritative frame on arrival.
   Hides the whole round-trip for the dominant streaming case. Most impressive,
   most involved; needs a thin cooperating shim.
