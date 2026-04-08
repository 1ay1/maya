# Rendering Modes

maya provides four rendering modes, each suited to different use cases. Choose
the one that matches your application's needs.

## Overview

| Mode | Function | Screen | Event Loop | Use Case |
|------|----------|--------|------------|----------|
| **Fullscreen** | `run({.mode = Mode::Fullscreen})` | Alt screen | Yes | Interactive TUIs, dashboards |
| **Inline** | `run({.mode = Mode::Inline})` | Scrollback | Yes | Claude Code-style apps |
| **Live** | `live()` | Scrollback | Timer-based | Progress bars, streaming output |
| **Canvas** | `canvas_run()` | Alt screen | Yes | Games, animations, visualizations |
| **One-shot** | `print()` | Scrollback | No | CLI output, reports, status cards |

## run() — Fullscreen Interactive Apps

The primary API for interactive terminal applications. Takes over the terminal
with the alternate screen buffer, handles input, and re-renders on events or
at a fixed frame rate.

### Signature

```cpp
// With explicit configuration
template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run(RunConfig cfg, EventFn&& event_fn, RenderFn&& render_fn);

// With defaults (dark theme, no mouse, alt screen)
template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run(EventFn&& event_fn, RenderFn&& render_fn);
```

### RunConfig

```cpp
struct RunConfig {
    std::string_view title      = "";              // Terminal window title
    int              fps        = 0;               // 0 = event-driven, >0 = continuous
    bool             mouse      = false;           // Enable mouse reporting
    Mode             mode       = Mode::Fullscreen;// Rendering mode
    Theme            theme      = theme::dark;     // Color theme
};
```

### Event Function

Two signatures are accepted:

```cpp
// Bool return: false = quit
[&](const Event& ev) -> bool { return !key(ev, 'q'); }

// Void return: call maya::quit() to exit
[&](const Event& ev) -> void { if (key(ev, 'q')) quit(); }
```

### Render Function

Two signatures are accepted:

```cpp
// No context — simple
[&]() -> Element { return v(t<"Hello">).build(); }

// With context — access terminal size and theme
[&](const Ctx& ctx) -> Element {
    int w = ctx.size.width.value;
    return v(text("Width: " + std::to_string(w))).build();
}
```

The `Ctx` struct:

```cpp
struct Ctx {
    Size  size;   // Current terminal dimensions
    Theme theme;  // Active color theme
};
```

### Event-Driven vs Continuous

- **`fps = 0`** (default): Only re-renders when an event arrives (key press,
  mouse move, resize). Minimal CPU usage. Best for static or user-driven UIs.

- **`fps = 30`** (or any positive value): Re-renders at the given frame rate
  regardless of input. Required for animations, timers, live data. The render
  function is called every frame; the event handler is called when events arrive.

### Example: Fullscreen Counter

```cpp
Signal<int> count{0};

run(
    {.title = "counter"},
    [&](const Event& ev) {
        on(ev, '+', '=', [&] { count.update([](int& n) { ++n; }); });
        on(ev, '-', '_', [&] { count.update([](int& n) { --n; }); });
        return !key(ev, 'q');
    },
    [&] {
        return (v(
            dyn([&] { return text("Count: " + std::to_string(count.get()),
                                  Style{}.with_bold()); }),
            t<"[+/-] change  [q] quit"> | Dim
        ) | pad<1>).build();
    }
);
```

### Inline Mode via run()

Set `mode = Mode::Inline` to render in the scrollback instead of the alt screen:

```cpp
run(
    {.mode = Mode::Inline},
    event_fn,
    render_fn
);
```

This gives you event handling with inline rendering — useful for TUIs that
should stay in the terminal history.

## live() — Timer-Based Inline Rendering

For animations and progress displays that render inline (in the terminal's
scrollback) without taking over the screen. No event handling — just a render
loop with a timer.

### Signature

```cpp
template <AnyLiveRenderFn RenderFn>
void live(LiveConfig cfg, RenderFn&& render_fn);
```

### LiveConfig

```cpp
struct LiveConfig {
    int   fps       = 30;   // Target frames per second
    int   max_width = 0;    // 0 = auto-detect terminal width
    bool  cursor    = false; // Show cursor during rendering
};
```

### Render Function

Two signatures:

```cpp
// With delta time (seconds since last frame)
[&](float dt) -> Element { ... }

// Without delta time
[&]() -> Element { ... }
```

### Stopping the Loop

Call `maya::quit()` from inside the render function:

```cpp
live({.fps = 30}, [&](float dt) {
    elapsed += dt;
    if (elapsed > 5.0f) quit();  // Stop after 5 seconds
    return text("Time: " + std::to_string(elapsed));
});
```

### Example: Progress Bar

```cpp
float progress = 0;
live({.fps = 30}, [&](float dt) {
    progress += dt * 0.2f;
    if (progress >= 1.0f) quit();

    int filled = static_cast<int>(progress * 40);
    std::string bar(filled, '#');
    bar += std::string(40 - filled, '.');

    return (v(
        text("Installing...") | Bold,
        text("[" + bar + "] " + std::to_string(int(progress * 100)) + "%")
    ) | pad<0, 1>).build();
});
```

### How It Works

`live()` renders each frame by:
1. Building the element tree from your render function
2. Laying out and painting to a canvas
3. Serializing to ANSI escape sequences
4. Moving the cursor up to overwrite the previous frame
5. Writing the new frame
6. Sleeping until the next frame time

The cursor is hidden during rendering and restored on exit. Output stays in the
terminal scrollback — it doesn't use the alt screen.

## canvas_run() — Imperative Canvas Painting

For maximum control: direct cell-level painting on a double-buffered canvas.
Best for games, particle systems, complex visualizations, and anything that
needs per-cell control.

### Signature

```cpp
Status canvas_run(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint
);
```

### CanvasConfig

```cpp
struct CanvasConfig {
    int         fps        = 60;              // Target frame rate
    bool        mouse      = false;           // Enable mouse reporting
    Mode        mode       = Mode::Fullscreen;// Rendering mode
    std::string title;                        // Terminal window title
};
```

### Callbacks

**on_resize(StylePool& pool, int w, int h)**
Called at startup and after each terminal resize. The style pool is cleared
before the call — re-intern all your styles here:

```cpp
[&](StylePool& pool, int W, int H) {
    // Pre-intern styles (compact uint16_t IDs for canvas cells)
    style_bold = pool.intern(Style{}.with_bold().with_fg(Color::green()));
    style_dim  = pool.intern(Style{}.with_dim().with_fg(Color::gray()));

    // Rebuild size-dependent state
    particles.resize(W * H);
}
```

**on_event(const Event& ev) -> bool**
Same as `run()` — return false to quit:

```cpp
[&](const Event& ev) -> bool {
    if (key(ev, 'q')) return false;
    on(ev, 'p', [&] { paused = !paused; });
    return true;
}
```

**on_paint(Canvas& canvas, int w, int h)**
Called every frame. The canvas is pre-cleared. Paint your frame:

```cpp
[&](Canvas& canvas, int W, int H) {
    for (auto& particle : particles) {
        canvas.set(particle.x, particle.y, particle.glyph, particle.style_id);
    }
    canvas.write_text(0, H - 1, "status bar", bar_style);
}
```

### Return Value

`canvas_run()` returns a `Status` (`Result<void>`). Check for errors:

```cpp
auto result = canvas_run(config, on_resize, on_event, on_paint);
if (!result) {
    std::println(std::cerr, "maya: {}", result.error().message);
    return 1;
}
```

### Example: Starfield

```cpp
struct Star { float x, y, speed; };
std::vector<Star> stars;
uint16_t star_style;

auto result = canvas_run(
    {.fps = 60, .title = "starfield"},

    [&](StylePool& pool, int W, int H) {
        star_style = pool.intern(Style{}.with_bold().with_fg(Color::white()));
        stars.clear();
        for (int i = 0; i < 200; ++i)
            stars.push_back({randf(0, W), randf(0, H), randf(0.5f, 3.0f)});
    },

    [&](const Event& ev) { return !key(ev, 'q'); },

    [&](Canvas& canvas, int W, int H) {
        for (auto& s : stars) {
            s.x -= s.speed;
            if (s.x < 0) { s.x = W; s.y = randf(0, H); }
            canvas.set(int(s.x), int(s.y), U'*', star_style);
        }
    }
);
```

## print() — One-Shot Output

Render an element tree to stdout and return. No event loop, no terminal control.
Perfect for CLI tools that want styled output:

```cpp
void print(const Element& root);           // Auto-detect terminal width
void print(const Element& root, int width); // Explicit width
```

### Example

```cpp
constexpr auto card = v(
    t<"Build Status"> | Bold | Fg<100, 180, 255>,
    t<"">,
    h(t<"Tests:">  | Dim, t<" 142 passed"> | Fg<80, 220, 120>),
    h(t<"Lint:">   | Dim, t<" 0 warnings"> | Fg<80, 220, 120>),
    h(t<"Bundle:"> | Dim, t<" 2.4 MB"> | Fg<240, 200, 60>)
) | border_<Round> | bcol<60, 65, 80> | pad<1>;

print(card.build());
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

## Inline Scrollback Preservation

When building inline (non-fullscreen) UIs — particularly AI agent sessions,
multi-step build pipelines, or any workflow where components complete and new
ones appear — preserving the terminal scrollback is critical.  The user should
be able to scroll up and see the full history of what happened: expanded diffs,
tool output, test results, etc.

### The Problem: Content Shrinkage Destroys Scrollback

The inline renderer works by overwriting its output in place each frame: it
moves the cursor up to the top of the previous frame, writes the new frame,
and erases any leftover lines below.

This works perfectly when content height stays the same or grows.  But when
content **shrinks** — e.g. a tool card collapses from 20 rows (showing a full
diff) to 2 rows (just a status line) — the old expanded content at those
terminal rows is **overwritten** with the shorter content, and the leftover
lines are **erased** with `\x1b[2K`.  The old diff is gone from the terminal
buffer entirely.  The user cannot scroll up to see it.

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

### The Solution: Two-Part Fix

Maya solves this at **both** the framework and application levels.

#### 1. Framework: Row-Hash Committed Scrollback

The inline renderer computes a fast hash (FNV-1a over packed 64-bit cells) for
every canvas row each frame.  It compares these hashes against the previous
frame to find the **stable prefix** — the longest run of rows from the top that
are identical between frames.

Stable rows are **committed** to scrollback: the cursor is never moved above
them and they are never overwritten.  Only the "live" region below the
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

Once committed, a row stays committed for the entire inline session.  Even if
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

The framework's row-hash comparison works best when content **grows
monotonically** — each new component adds rows below existing ones, and
completed components keep their content visible.

This mirrors how Claude Code (built on Ink) works:

- A completed Read card keeps its file preview visible
- A completed Edit card keeps its diff visible with a ✓ header
- A completed Bash card keeps its output visible with exit code
- Only the header styling changes (spinner → checkmark)

**Do this:**

```cpp
// Tool status changes but content stays visible
if (phase_timer > 2.0f) {
    edit_status = TaskStatus::Completed;  // header shows ✓
    // DiffView stays in the tree — height doesn't change
}
```

**Don't do this:**

```cpp
// ❌ Dramatic collapse — destroys scrollback content
if (phase_timer > 2.0f) {
    edit_status = TaskStatus::Completed;
    tool_collapsed = true;  // Hides DiffView, height drops 15+ rows
}
```

If you need user-toggleable collapse, use key bindings:

```cpp
if (key(ev, '2')) tool_collapsed = !tool_collapsed;
```

This way, the user controls when to collapse — the framework doesn't do it
automatically during the session flow.

### How It All Fits Together

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

### Limitations

- **Hash collisions**: The FNV-1a row hash has a theoretical collision risk.
  In practice, terminal content collisions are astronomically unlikely (one in
  ~2^64 per row pair per frame).  A false match would cause one row to be
  skipped for one frame — self-correcting on the next frame when the hash
  changes.

- **Content above committed boundary can't update**: If you change content
  at a row that's already committed (e.g. updating an old tool card header),
  the terminal won't reflect the change.  The committed row retains what was
  originally rendered.  This is by design — it's the scrollback preservation
  guarantee.

- **Very tall content**: When content exceeds the terminal height, the top
  rows are cropped via `skip_rows`.  Rows that were visible and committed but
  get cropped remain in the terminal's scrollback from when they were written.

## Choosing the Right Mode

```
Need interactivity?
├── Yes: Need per-cell control?
│   ├── Yes → canvas_run()     (games, animations, visualizations)
│   └── No  → run()            (dashboards, forms, menus)
│       └── Want scrollback output? → run({.mode = Mode::Inline}, ...)
└── No: Need animation?
    ├── Yes → live()     (progress bars, streaming output)
    └── No  → print()          (CLI reports, status cards)
```

## quit() — Global Exit

```cpp
void maya::quit() noexcept;
```

Call from anywhere — event handlers, render functions, signal effects — to
schedule a clean exit after the current frame. Thread-local, safe to call
from any context.

```cpp
// In event handler
on(ev, 'q', [] { quit(); });

// In live render
live({}, [&](float dt) {
    if (done) quit();
    return text("...");
});

// In a signal effect
auto fx = effect([&] {
    if (count.get() > 100) quit();
});
```
