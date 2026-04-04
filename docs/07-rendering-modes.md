# Rendering Modes

maya provides four rendering modes, each suited to different use cases. Choose
the one that matches your application's needs.

## Overview

| Mode | Function | Screen | Event Loop | Use Case |
|------|----------|--------|------------|----------|
| **Fullscreen** | `run()` | Alt screen | Yes | Interactive TUIs, dashboards |
| **Inline** | `inline_run()` | Scrollback | Timer-based | Progress bars, streaming output |
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
    std::string_view title      = "";           // Terminal window title
    int              fps        = 0;            // 0 = event-driven, >0 = continuous
    bool             mouse      = false;        // Enable mouse reporting
    bool             alt_screen = true;         // Use alt screen buffer
    Theme            theme      = theme::dark;  // Color theme
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

Set `alt_screen = false` to render in the scrollback instead of the alt screen:

```cpp
run(
    {.alt_screen = false},
    event_fn,
    render_fn
);
```

This gives you event handling with inline rendering — useful for TUIs that
should stay in the terminal history.

## inline_run() — Timer-Based Inline Rendering

For animations and progress displays that render inline (in the terminal's
scrollback) without taking over the screen. No event handling — just a render
loop with a timer.

### Signature

```cpp
template <AnyInlineRenderFn RenderFn>
void inline_run(InlineConfig cfg, RenderFn&& render_fn);
```

### InlineConfig

```cpp
struct InlineConfig {
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
inline_run({.fps = 30}, [&](float dt) {
    elapsed += dt;
    if (elapsed > 5.0f) quit();  // Stop after 5 seconds
    return text("Time: " + std::to_string(elapsed));
});
```

### Example: Progress Bar

```cpp
float progress = 0;
inline_run({.fps = 30}, [&](float dt) {
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

`inline_run()` renders each frame by:
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
    int         fps        = 60;     // Target frame rate
    bool        mouse      = false;  // Enable mouse reporting
    bool        alt_screen = true;   // Use alt screen buffer
    std::string title;               // Terminal window title
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

## Choosing the Right Mode

```
Need interactivity?
├── Yes: Need per-cell control?
│   ├── Yes → canvas_run()     (games, animations, visualizations)
│   └── No  → run()            (dashboards, forms, menus)
│       └── Want scrollback output? → run({.alt_screen = false}, ...)
└── No: Need animation?
    ├── Yes → inline_run()     (progress bars, streaming output)
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

// In inline_run render
inline_run({}, [&](float dt) {
    if (done) quit();
    return text("...");
});

// In a signal effect
auto fx = effect([&] {
    if (count.get() > 100) quit();
});
```
