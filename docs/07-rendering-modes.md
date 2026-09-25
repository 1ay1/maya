# Rendering Modes

There is one way to run an interactive maya program — `run<P>(Options)` — and
two places it can draw: the alternate screen (**fullscreen**) or the terminal's
normal scrollback (**inline**). Static output that needs no runtime at all goes
through `print()` / `render_to_string()`.

## Overview

| Mode | How | Screen | Runtime | Use Case |
|------|-----|--------|---------|----------|
| **Fullscreen** | `run<P>({.mode = Mode::Fullscreen})` (default) | Alt screen | jaal | Interactive TUIs, dashboards, games, pixel art |
| **Inline** | `run<P>({.mode = Mode::Inline})` | Scrollback | jaal | Claude Code-style sessions, progress bars, streaming output |
| **Static** | `print(el)` / `render_to_string(el, w)` | Scrollback / string | none | CLI output, reports, status cards |

!!! note "What changed"
    The four separate loops — `run(cfg, event_fn, render_fn)`, `live()`,
    `canvas_run()` and the old `run<P>(RunConfig)` — are gone. A progress bar
    is now a program run with `Mode::Inline` and a `Sub::every`; a canvas demo
    is a program whose `view()` returns `pixels(img)` or `glyphs(g)`;
    `RunConfig` is now `Options`.

## run\<P\>() — Interactive Programs

A program is a jaal program whose `view()` returns an `Element`:

```cpp
#include <maya/app.hpp>
using namespace maya;
using namespace maya::dsl;

struct Counter {
    struct Model { int count = 0; };
    struct Inc {}; struct Dec {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Inc)  { ++m.count; return {}; }
    static Cmd update(Model& m, Dec)  { --m.count; return {}; }
    static Cmd update(Model&,   Quit) { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return text("count: " + std::to_string(m.count), Style{}.with_bold());
    }

    static Sub subscribe(const Model&) {
        return keys<Sub>({{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}});
    }
};

int main() { return run<Counter>({.title = "counter"}); }
```

`run<P>()` opens the terminal and hands it to jaal's loop; it returns the
exit code passed to `Cmd::quit`. The mode is just an option — the same program
runs fullscreen or inline unchanged.

### Options

```cpp
struct Options {
    std::string_view title        = "";                  // Terminal window title
    int              fps          = 0;                   // 0 = event-driven, >0 = continuous
    bool             mouse        = false;               // Enable mouse reporting
    bool             hover_motion = false;               // Also report bare motion (hover)
    Mode             mode         = Mode::Fullscreen;    // Fullscreen or Inline
    RenderBackend    backend      = RenderBackend::Ansi; // Frame transport
    Theme            theme        = theme::native;       // Colour theme
    bool             enhanced_keyboard = true;           // Kitty keyboard protocol
};
```

### Event-Driven vs Continuous

- **`fps = 0`** (default): a frame is drawn only when something changes — a
  message was handled, the terminal resized. A still screen costs nothing.
- **`fps = N`**: redraw continuously at up to N frames per second, regardless
  of input.

Prefer driving animation from the model: put a `Sub::every(16ms, Tick{})` in
`subscribe()`, advance state in `update(Model&, Tick)`, and drop the
subscription when the animation stops. That keeps `fps = 0` and makes "idle"
genuinely idle:

```cpp
static Sub subscribe(const Model& m) {
    if (m.paused) return keys<Sub>({{'p', Pause{}}, {'q', Quit{}}});
    return Sub::batch(
        Sub::every(16ms, Tick{}),
        keys<Sub>({{'p', Pause{}}, {'q', Quit{}}}));
}
```

### Frame Flow Control

maya never draws more than **one frame ahead of the terminal**. Each frame
ends with a Device Status Report query (`CSI 5 n`); the terminal answers once
it has parsed everything before it, which acknowledges that the frame reached
the glass. While a frame is unacknowledged, your program keeps running —
messages are handled and the model updates — but nothing new is drawn. The
next frame drawn is the **latest** state.

Why it matters: over SSH the pty drains into sshd instantly, and the real
bottleneck (the network, the remote terminal's parser) sits behind buffers
that can hold megabytes. A program that draws as fast as the pty accepts fills
them, and every keypress — including `q` — then waits behind seconds of stale
frames. With flow control, a burst of input costs one frame rather than a
queue of them, and the frame rate settles at exactly what the link sustains.
Locally the ack returns in well under a millisecond, so nothing is throttled.

A terminal that never answers (a dumb pipe, a very old emulator) is detected
by timeout and flow control switches itself off. You don't configure any of
this; `fps` is an upper bound, not a promise.

## Fullscreen Mode

`Mode::Fullscreen` (the default) switches to the alternate screen, so the
program owns the whole window and the user's shell history reappears
untouched on exit. Frames are diffed cell by cell; only changed cells are
written.

Games, simulations and pixel graphics are ordinary fullscreen programs.
There is no separate canvas loop: the model holds the state, `update(Tick)`
advances it, and `view()` draws it — for example with an `Image`:

```cpp
#include <maya/app.hpp>
#include <maya/element/pixels.hpp>
using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

struct Plasma {
    struct Model { float t = 0; int w = 80, h = 24; };
    struct Tick {}; struct Resized { int w, h; }; struct Quit {};
    using Msg = std::variant<Tick, Resized, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Tick)      { m.t += 0.016f; return {}; }
    static Cmd update(Model& m, Resized r) { m.w = r.w; m.h = r.h; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    static Element view(const Model& m) {
        Image img(m.w, m.h * 2);                        // half blocks: 2 px per row
        img.fill_rows([&](int x, int y) -> Rgb {
            const float v = std::sin(x * 0.1f + m.t) + std::sin(y * 0.1f - m.t);
            const auto c = static_cast<uint8_t>(127 + 63 * v);
            return Rgb{c, uint8_t(255 - c), 200};
        });
        return pixels(std::move(img));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            keys<Sub>({{'q', Quit{}}}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resized{r.width.value, r.height.value};
            }));
    }
};

int main() { return run<Plasma>({.title = "plasma"}); }
```

See `examples/doom_fire.cpp`, `mandelbrot.cpp`, `raymarch.cpp` and
[Canvas API](08-canvas-api.md) for `Image`, `pixels()` and `glyphs()`.

## Inline Mode

`Mode::Inline` renders into the terminal's normal scrollback instead of the
alt screen. The program's output stays in the terminal history after it
exits — ideal for agent sessions, build pipelines, and progress displays.

```cpp
struct Progress {
    struct Model { int pct = 0; std::chrono::milliseconds step{30}; };
    struct Tick {};
    using Msg = std::variant<Tick>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Tick) {
        if (++m.pct >= 100) return Cmd::quit(0);
        return {};
    }

    static Element view(const Model& m) {
        return h(text("Downloading "),
                 text(std::string(m.pct / 5, '#') + std::string(20 - m.pct / 5, '.')),
                 text(" " + std::to_string(m.pct) + "%")).build();
    }

    static Sub subscribe(const Model& m) { return Sub::every(m.step, Tick{}); }
};

int main() { return run<Progress>({.mode = Mode::Inline}); }
```

(See `examples/inline_progress.cpp`.) Everything else — events, commands,
subscriptions — works exactly as in fullscreen.

### Inline Scrollback Preservation

When building inline UIs — particularly AI agent sessions, multi-step build
pipelines, or any workflow where components complete and new ones appear —
preserving the terminal scrollback is critical. The user should be able to
scroll up and see the full history of what happened: expanded diffs, tool
output, test results, etc.

#### The Problem: Content Shrinkage Destroys Scrollback

The inline renderer works by overwriting its output in place each frame: it
moves the cursor up to the top of the previous frame, writes the new frame,
and erases any leftover lines below.

This works perfectly when content height stays the same or grows. But when
content **shrinks** — e.g. a tool card collapses from 20 rows (showing a full
diff) to 2 rows (just a status line) — the old expanded content at those
terminal rows is **overwritten** with the shorter content, and the leftover
lines are **erased** with `\x1b[2K`. The old diff is gone from the terminal
buffer entirely. The user cannot scroll up to see it.

```
Frame N (tool running — 20 rows):
┌─────────────────────────────────────┐
│ ▸ Edit  src/middleware/auth.ts      │  ← header
│   src/middleware/auth.ts            │  ← breadcrumb
│   -import session from 'express-…  │  ← diff line 1
│   +import jwt from 'jsonwebtoken'; │  ← diff line 2
│   …16 more diff lines…             │
│   ◐ running                        │  ← spinner
└─────────────────────────────────────┘

Frame N+1 (tool done — 2 rows):
┌─────────────────────────────────────┐
│ ✓ Edit  src/middleware/auth.ts  +18 -14 │  ← collapsed
│                                         │  ← next tool starts…
└─────────────────────────────────────┘
↑ Cursor moved up 19 rows, overwrote everything.
  Rows 3–20 erased.  Diff is gone from scrollback.
```

Maya solves this at **both** the framework and application levels.

#### 1. Framework: Row-Hash Committed Scrollback

The inline renderer computes a fast hash (FNV-1a over packed 64-bit cells) for
every canvas row each frame. It compares these hashes against the previous
frame to find the **stable prefix** — the longest run of rows from the top that
are identical between frames.

Stable rows are **committed** to scrollback: the cursor is never moved above
them and they are never overwritten. Only the "live" region below the
committed area is re-rendered each frame.

```
Canvas row 0:  [User message]       ← stable, committed (never touched)
Canvas row 1:  [Context pills]      ← stable, committed
Canvas row 2:  [Thinking block]     ← stable, committed
Canvas row 3:  [Tool card header]   ← stable, committed
Canvas row 4:  [  diff line 1]      ← stable, committed
…
Canvas row 18: [  diff line 15]     ← stable, committed
Canvas row 19: [Spinner / status]   ← CHANGING → live region starts here
Canvas row 20: [Status bar]         ← CHANGING → live
```

Once committed, a row stays committed for the entire inline session. Even if
the canvas content at that position later changes (e.g. the tool card header
switches from a spinner to a checkmark), the committed row in the terminal
retains its original content — which is exactly what scrollback preservation
means.

The live region (everything below the committed boundary) is managed normally:
overwritten in place each frame, with leftover lines erased when it shrinks.

**Key implementation details:**

- `committed_height_` is monotonically increasing — rows are never un-committed
- Row hashes use FNV-1a for fast comparison with extremely low collision risk
- The `serialize()` call is passed `live_start` to skip committed rows entirely
- `prev_live` tracks the live area height for correct cursor movement

#### 2. Application: Content Should Only Grow

The row-hash comparison works best when content **grows monotonically** —
each new component adds rows below existing ones, and completed components
keep their content visible.

This mirrors how Claude Code (built on Ink) works:

- A completed Read card keeps its file preview visible
- A completed Edit card keeps its diff visible with a ✓ header
- A completed Bash card keeps its output visible with exit code
- Only the header styling changes (spinner → checkmark)

**Do this:**

```cpp
// Tool status changes but content stays visible
static Cmd update(Model& m, EditDone) {
    m.edit_status = TaskStatus::Completed;  // header shows ✓
    return {};                              // DiffView stays in the tree
}
```

**Don't do this:**

```cpp
// ❌ Dramatic collapse — destroys scrollback content
static Cmd update(Model& m, EditDone) {
    m.edit_status    = TaskStatus::Completed;
    m.tool_collapsed = true;   // hides DiffView, height drops 15+ rows
    return {};
}
```

If you need user-toggleable collapse, bind it to a key so the user decides
when it happens:

```cpp
static Cmd update(Model& m, ToggleCollapse) { m.tool_collapsed = !m.tool_collapsed; return {}; }

static Sub subscribe(const Model&) { return keys<Sub>({{'2', ToggleCollapse{}}}); }
```

#### How It All Fits Together

```
Session start:
  committed = 0, live = all rows
  └─ Every row is overwritten each frame (normal)

After 5 stable frames:
  committed = 12, live = rows 12+
  └─ Rows 0–11 (user msg, context, thinking) are locked in scrollback

Tool card runs for 2 seconds:
  committed = 12, live = rows 12+ (tool header + diff change due to spinner)
  └─ When spinner stops → tool body rows become stable → committed grows to 30

New tool starts:
  committed = 30, live = rows 30+ (new tool header + body)
  └─ All previous tool output (rows 0–29) locked in scrollback forever

User scrolls up in terminal:
  └─ Sees full diffs, file contents, test output — all preserved
```

#### Limitations

- **Hash collisions**: The FNV-1a row hash has a theoretical collision risk.
  In practice, terminal content collisions are astronomically unlikely (one in
  ~2^64 per row pair per frame). A false match would cause one row to be
  skipped for one frame — self-correcting on the next frame when the hash
  changes.

- **Content above committed boundary can't update**: If you change content
  at a row that's already committed (e.g. updating an old tool card header),
  the terminal won't reflect the change. The committed row retains what was
  originally rendered. This is by design — it's the scrollback preservation
  guarantee.

- **Very tall content**: When content exceeds the terminal height, the top
  rows are cropped via `skip_rows`. Rows that were visible and committed but
  get cropped remain in the terminal's scrollback from when they were written.

### Scrollback Effects

Most programs get correct scrollback for free from the row-hash mechanism.
Programs that manage their own sealed history (agent sessions that virtualise
or trim old turns) drive it explicitly with **terminal effects** returned from
`update()`. Like every effect, each one must be listed in the program's `Cmd`
row (or use `terminal_cmd<Msg>`, which lists them all). All of them are
no-ops in fullscreen.

| Effect (Cmd row) | Payload | Use |
|------------------|---------|-----|
| `commit_scrollback` | `commit_from<Cmd>(ledger.harvest())` | Commit rows of the last frame before `view()` returns a shorter tree, so the shrink isn't read as rows removed and erased. |
| `commit_overflow` | `CommitOverflow{}` | Commit every row of the last frame that has provably scrolled past the viewport; maya derives the safe row count itself. |
| `force_redraw` | `ForceRedraw{}` | Soft repaint of the live viewport on the next frame. |
| `reset_inline` | `ResetInline{}` | Hard inline reset (destructive scrollback wipe) — wholesale model swaps only. |

```cpp
struct Chat {
    struct Model { ScrollbackLedger frozen; std::vector<Turn> live; /* … */ };
    // …
    using Cmd = jaal::Cmd<Msg, commit_scrollback, commit_overflow>;

    static Cmd update(Model& m, SealTurns) {
        // Move finished turns out of the live tree; the ledger recorded the
        // rows maya painted for them, so the commit can't drift from the wire.
        seal_finished(m);
        return commit_from<Cmd>(m.frozen.harvest());   // empty debt → no-op
    }
};
```

`commit_scrollback` deliberately carries a typed `ScrollbackDebt`, not an
`int`: a debt can only be minted by `ScrollbackLedger::harvest()`, whose rows
were recorded by maya's own paint pass, so a program structurally can't commit
a row count that differs from what is on screen. Render the sealed prefix via
`ledger_ref` / `Conversation::Config::ledger` so the paint pass records it.

## print() / render_to_string() — Static Output

Render an element tree once, with no runtime, no event loop and no terminal
control. Perfect for CLI tools that want styled output. Both live in
`<maya/print.hpp>` (included by `<maya/maya.hpp>`):

```cpp
void        print(const Element& root);               // auto-detect terminal width
void        print(const Element& root, int width);    // explicit width
std::string render_to_string(const Element& root, int width = 80);  // ANSI string, no I/O
```

`render_to_string` is maya's `renderToString`: useful for tests, logs, or
embedding styled output in another tool.

### Example

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    constexpr auto card = v(
        t<"Build Status"> | Bold | Fg<100, 180, 255>,
        t<"">,
        h(t<"Tests:">  | Dim, t<" 142 passed"> | Fg<80, 220, 120>),
        h(t<"Lint:">   | Dim, t<" 0 warnings"> | Fg<80, 220, 120>),
        h(t<"Bundle:"> | Dim, t<" 2.4 MB"> | Fg<240, 200, 60>)
    ) | border_<Round> | bcol<60, 65, 80> | pad<1>;

    print(card.build());
}
```

Output (with ANSI colors in a real terminal):
```
╭──────────────────────────╮
│ Build Status             │
│                          │
│ Tests:  142 passed       │
│ Lint:   0 warnings       │
│ Bundle: 2.4 MB           │
╰──────────────────────────╯
```
