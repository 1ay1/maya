# maya — A C++26 Type-Safe TUI Library

maya is a compile-time, type-safe terminal UI library for C++26. It combines a
declarative DSL with a flexbox layout engine and high-performance
SIMD-accelerated rendering to produce beautiful, fast TUIs with minimal
ceremony.

maya is to [jaal](../third_party/jaal) what Ink is to React: jaal runs the
program — model, messages, `update`, effects (`Cmd`) and subscriptions
(`Sub`) — and maya draws it. There is one way to run an app, and it is the
same for a counter, a dashboard, a fractal zoomer, or an inline progress bar.

## Philosophy

**Impossible states don't compile.** maya's DSL uses C++26 template
metaprogramming and type-state machines to catch errors at compile time. You
can't set a border color without first declaring a border style — the compiler
rejects it. Padding values can't be negative. Style modifiers only compose where
they make sense.

**One program shape.** A maya app is a struct with a `Model`, a `Msg` variant,
one `update` overload per message, a pure `view`, and a `subscribe` that says
which events and timers it listens to. `run<P>()` does the rest:

```cpp
#include <maya/host/run.hpp>
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
        return v(
            text("Count: " + std::to_string(m.count)) | Bold | Fg<100, 200, 255>,
            t<"[+/-] change  [q] quit"> | Dim
        ) | border_<Round> | bcol<50, 55, 70> | pad<1>;
    }

    static Sub subscribe(const Model&) {
        return keys<Sub>({{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}});
    }
};

int main() { return run<Counter>({.title = "counter"}); }
```

**Everything the model says, the view says.** No globals, no hidden widget
state, no callbacks writing into captured variables. `view()` is a pure
function of the model, so it is trivially testable. Animation is a
`Sub::every` in `subscribe()` — which is also where it stops, so a still
screen costs nothing.

**Say what you use.** The rows of `Cmd` and `Sub` list the terminal effects
and event sources a program needs (`jaal::Sub<Msg, on_key, on_mouse>`,
`jaal::Cmd<Msg, set_title>`). Asking for something the host can't provide is
a compile error, not a runtime surprise.

**Performance by default.** Styles are interned into 16-bit IDs. Cells are
packed into 64-bit values. Frame diffing uses SIMD (AVX-512, AVX2, SSE2, NEON)
to compare thousands of cells in microseconds. The layout engine is a
single-pass flexbox solver with no heap allocation in the hot path.

**Composition over inheritance.** No virtual classes, no CRTP hierarchies. UI
trees are plain data — `std::variant` of `TextElement`, `BoxElement`, and
`ElementList`. The DSL layer is pure templates that produce these data types.

> **Coming from the old API?** The callback loops (`run(event_fn, render_fn)`,
> `live()`, `canvas_run()`), `maya::Cmd`/`maya::Sub`, `key_map`, and
> `RunConfig` are gone. Everything is now a program run by `run<P>(Options)`;
> a canvas demo is a program whose view is `pixels(img)`, and a live progress
> line is a program run with `.mode = Mode::Inline`.

## What maya Gives You

| Feature | Description |
|---------|-------------|
| **Compile-time DSL** | `t<"Hello"> \| Bold \| Fg<100,180,255>` — text, style, layout as template parameters |
| **Flexbox layout** | Row/column stacking, padding, margin, grow/shrink, gap, alignment, wrapping |
| **Borders** | Single, Double, Round, Bold, Classic, Arrow — with colors, titles, per-side control |
| **One program shape** | `Model` + `Msg` + `update` + `view` + `subscribe`, run by `run<P>()` on jaal |
| **Two modes** | `Mode::Fullscreen` (alternate screen) or `Mode::Inline` (lives in scrollback); plus `print()` for one-shot output |
| **Rich input** | Keys, mouse clicks/movement/scroll, paste, focus, resize — each an event source in the `Sub` row |
| **Terminal effects** | Title, clipboard, scrollback commits, mouse toggling, suspend-for-`$EDITOR` — as `Cmd` rows |
| **Pixel graphics** | `Image` + `pixels()` for half-block RGB art, `Glyphs` + `glyphs()` for coloured character art |
| **Themes** | 24-slot color themes with dark/light built-ins and compile-time derivation |
| **Unicode** | Full UTF-8 support with CJK wide-character handling and braille sub-cell graphics |
| **SIMD diffing** | O(N/8) frame comparison for minimal terminal writes |

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    User Code                         │
│  struct App { Model, Msg, update, view, subscribe }  │
│  int main() { return run<App>({...}); }              │
├─────────────────────────────────────────────────────┤
│         jaal (the runtime — third_party/jaal)        │
│  The loop: messages, Cmd effects, Sub diffing,       │
│  timers, worker threads, shutdown                    │
├─────────────────────────────────────────────────────┤
│              maya::app (app.hpp)                     │
│  run<P>(Options): opens the terminal, hands it to    │
│  jaal. Event sources (on_key, on_mouse, …) and       │
│  terminal effects (set_title, suspend, …)            │
├─────────────────────────────────────────────────────┤
│                 DSL Layer (dsl.hpp)                   │
│  t<>, text(), v(), h(), pipes; type-state validation │
├─────────────────────────────────────────────────────┤
│              Element Layer (element/)                 │
│  Element = variant<BoxElement, TextElement, ElementList>│
│  pixels(Image), glyphs(Glyphs), widgets              │
├─────────────────────────────────────────────────────┤
│              Layout Engine (layout/)                  │
│  Pure C++ flexbox solver — no dependencies            │
├─────────────────────────────────────────────────────┤
│             Render Pipeline (render/)                 │
│  Canvas → StylePool → SIMD Diff → ANSI Serialize     │
├─────────────────────────────────────────────────────┤
│             Terminal Layer (terminal/)                │
│  Raw mode, alt screen, input parsing, ANSI output    │
└─────────────────────────────────────────────────────┘
```

## Quick Taste

### Static output (no runtime)

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    auto ui = v(
        t<"System Status"> | Bold | Fg<220, 220, 240>,
        h(t<"CPU:"> | Dim, t<" 42%"> | Bold | Fg<80, 220, 120>),
        h(t<"Mem:"> | Dim, t<" 8.2G"> | Bold | Fg<180, 130, 255>)
    ) | border_<Round> | bcol<60, 65, 80> | pad<1>;

    print(ui);
}
```

### A timer (animation is a subscription)

```cpp
using namespace std::chrono_literals;

struct Clock {
    struct Model { int ticks = 0; bool paused = false; };
    struct Tick {}; struct Pause {}; struct Quit {};
    using Msg = std::variant<Tick, Pause, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Tick)  { ++m.ticks; return {}; }
    static Cmd update(Model& m, Pause) { m.paused = !m.paused; return {}; }
    static Cmd update(Model&,   Quit)  { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return text("Ticks: " + std::to_string(m.ticks)) | Bold;
    }
    static Sub subscribe(const Model& m) {
        auto k = keys<Sub>({{' ', Pause{}}, {'q', Quit{}}});
        if (m.paused) return k;                       // no timer: nothing runs
        return Sub::batch(std::move(k), Sub::every(100ms, Tick{}));
    }
    static bool subs_key(const Model& m) { return m.paused; }
};
```

### Inline progress (no alt screen)

The same shape, run with `run<Progress>({.mode = Mode::Inline})`: the view
renders into the normal scrollback, and the final frame stays on screen when
the program quits. See `examples/inline_progress.cpp`.

### Pixel graphics

```cpp
static Element view(const Model& m) {
    Image img(m.w, m.h);                              // h = 2 × rows (half blocks)
    img.fill_rows([&](int x, int y) -> Rgb {
        return {static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4), 128};
    });
    return pixels(std::move(img));
}
```

## Documentation Map

| Document | What You'll Learn |
|----------|-------------------|
| [Getting Started](01-getting-started.md) | Building, CMake setup, your first program, timers, input, pixels, inline mode |
| [The DSL](02-dsl.md) | Compile-time nodes, `t<>`, `v()`, `h()`, pipe operators |
| [Styling](03-styling.md) | Colors, text attributes, themes, compile-time style tags |
| [Layout](04-layout.md) | Flexbox model, direction, padding, grow, borders, alignment |
| [Runtime Content](05-runtime-content.md) | `text()`, `map()`, mixing static and dynamic content |
| [Event Handling](06-events.md) | Event sources, `keys<Sub>`, `Sub::on`, key predicates |
| [Rendering Modes](07-rendering-modes.md) | `run<P>()`, `Mode::Fullscreen` vs `Mode::Inline`, `print()` |
| [Canvas API](08-canvas-api.md) | Low-level painting, StylePool, cells, `Image`/`pixels()` |
| [Signals & Reactivity](09-signals.md) | `Signal<T>`, `Computed<T>`, `Effect`, `Batch` |
| [Examples Walkthrough](10-examples.md) | Annotated guide through the built-in examples |
| [API Reference](11-api-reference.md) | Complete type and function reference |
