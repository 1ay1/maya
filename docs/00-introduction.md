# maya — A C++26 Type-Safe TUI Library

maya is a compile-time, type-safe terminal UI library for C++26. It combines a
declarative DSL with a flexbox layout engine, reactive signals, and
high-performance SIMD-accelerated rendering to produce beautiful, fast TUIs with
minimal ceremony.

## Philosophy

**Impossible states don't compile.** maya's DSL uses C++26 template
metaprogramming and type-state machines to catch errors at compile time. You
can't set a border color without first declaring a border style — the compiler
rejects it. Padding values can't be negative. Style modifiers only compose where
they make sense.

**Zero ceremony.** A full interactive TUI is one function call:

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    Signal<int> n{0};
    run(
        [&](const Event& ev) {
            on(ev, '+', [&] { n.update([](int& x) { ++x; }); });
            return !key(ev, 'q');
        },
        [&] {
            return (v(
                dyn([&] { return text("Count: " + std::to_string(n.get())); }),
                t<"[+] increment  [q] quit"> | Dim
            ) | pad<1>).build();
        }
    );
}
```

**Performance by default.** Styles are interned into 16-bit IDs. Cells are
packed into 64-bit values. Frame diffing uses SIMD (AVX-512, AVX2, SSE2, NEON)
to compare thousands of cells in microseconds. The layout engine is a
single-pass flexbox solver with no heap allocation in the hot path.

**Composition over inheritance.** No virtual classes, no CRTP hierarchies. UI
trees are plain data — `std::variant` of `TextElement`, `BoxElement`, and
`ElementList`. The DSL layer is pure templates that produce these data types.
Runtime polymorphism happens through `std::visit` with the `overload{...}`
pattern.

## What maya Gives You

| Feature | Description |
|---------|-------------|
| **Compile-time DSL** | `t<"Hello"> \| Bold \| Fg<100,180,255>` — text, style, layout as template parameters |
| **Flexbox layout** | Row/column stacking, padding, margin, grow/shrink, gap, alignment, wrapping |
| **Borders** | Single, Double, Round, Bold, Classic, Arrow — with colors, titles, per-side control |
| **Reactive signals** | `Signal<T>`, `Computed<T>`, `Effect`, `Batch` — SolidJS-style fine-grained reactivity |
| **Four rendering modes** | `run()` (fullscreen/inline), `live()` (animated output), `canvas_run()` (imperative), `print()` (one-shot) |
| **Rich input** | Keys, mouse clicks/movement/scroll, paste, focus, resize — all via pattern matching |
| **Themes** | 24-slot color themes with dark/light built-ins and compile-time derivation |
| **Canvas API** | Direct cell-level painting for games, visualizations, and animations |
| **Unicode** | Full UTF-8 support with CJK wide-character handling and braille sub-cell graphics |
| **SIMD diffing** | O(N/8) frame comparison for minimal terminal writes |

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    User Code                         │
│  using namespace maya::dsl;                          │
│  auto ui = v(t<"Hello">, ...) | border_<Round>;     │
├─────────────────────────────────────────────────────┤
│                 DSL Layer (dsl.hpp)                   │
│  TextNode, BoxNode, DynNode, MapNode, RuntimeTextNode│
│  Compile-time type-state validation                  │
├─────────────────────────────────────────────────────┤
│              Element Layer (element/)                 │
│  Element = variant<BoxElement, TextElement, ElementList>│
│  BoxBuilder for runtime construction                 │
├─────────────────────────────────────────────────────┤
│              Layout Engine (layout/)                  │
│  Pure C++ flexbox solver — no dependencies            │
│  Direction, wrapping, alignment, sizing constraints   │
├─────────────────────────────────────────────────────┤
│             Render Pipeline (render/)                 │
│  Canvas → StylePool → SIMD Diff → ANSI Serialize     │
├─────────────────────────────────────────────────────┤
│             Terminal Layer (terminal/)                │
│  Raw mode, alt screen, input parsing, ANSI output    │
│  Type-state: Cooked → Raw → AltScreen                │
├─────────────────────────────────────────────────────┤
│               App Framework (app/)                   │
│  run(), live(), canvas_run(), print()                 │
│  Event dispatch, Signal integration, frame loop      │
└─────────────────────────────────────────────────────┘
```

## Quick Taste

### Static UI (compile-time)

```cpp
constexpr auto ui = v(
    t<"System Status"> | Bold | Fg<220, 220, 240>,
    h(t<"CPU:"> | Dim, t<" 42%"> | Bold | Fg<80, 220, 120>),
    h(t<"Mem:"> | Dim, t<" 8.2G"> | Bold | Fg<180, 130, 255>)
) | border_<Round> | bcol<60, 65, 80> | pad<1>;

print(ui.build());
```

### Interactive app (fullscreen)

```cpp
Signal<int> count{0};
run(
    {.title = "counter", .fps = 30},
    [&](const Event& ev) {
        on(ev, '+', '=', [&] { count.update([](int& n) { ++n; }); });
        return !key(ev, 'q');
    },
    [&](const Ctx& ctx) {
        return (v(
            dyn([&] { return text("Count: " + std::to_string(count.get()),
                                  Style{}.with_bold().with_fg(ctx.theme.primary)); }),
            t<"[+] count  [q] quit"> | Dim
        ) | border_<Round> | pad<1>).build();
    }
);
```

### Live progress (no alt screen)

```cpp
int n = 0;
live({.fps = 30}, [&](float dt) {
    if (++n > 200) quit();
    return (v(
        text("Installing... " + std::to_string(n) + "/200") | Bold,
        text(std::string(n / 10, '#') + std::string(20 - n / 10, '.'))
    ) | pad<0, 1>).build();
});
```

### Canvas animation (imperative)

```cpp
canvas_run(
    {.fps = 60, .mouse = true},
    [&](StylePool& pool, int w, int h) { /* intern styles */ },
    [&](const Event& ev) { return !key(ev, 'q'); },
    [&](Canvas& canvas, int w, int h) {
        canvas.set(w/2, h/2, U'*', style_id);
    }
);
```

## Documentation Map

| Document | What You'll Learn |
|----------|-------------------|
| [Getting Started](01-getting-started.md) | Installation, CMake setup, building your first app |
| [The DSL](02-dsl.md) | Compile-time nodes, `t<>`, `v()`, `h()`, pipe operators |
| [Styling](03-styling.md) | Colors, text attributes, themes, compile-time style tags |
| [Layout](04-layout.md) | Flexbox model, direction, padding, grow, borders, alignment |
| [Runtime Content](05-runtime-content.md) | `dyn()`, `text()`, `map()`, mixing static and dynamic |
| [Event Handling](06-events.md) | Keys, mouse, resize, `on()` helpers, event predicates |
| [Rendering Modes](07-rendering-modes.md) | `run()`, `live()`, `canvas_run()`, `print()` |
| [Canvas API](08-canvas-api.md) | Low-level painting, StylePool, cells, animations |
| [Signals & Reactivity](09-signals.md) | `Signal<T>`, `Computed<T>`, `Effect`, `Batch` |
| [Examples Walkthrough](10-examples.md) | Annotated guide through all 10 built-in examples |
| [API Reference](11-api-reference.md) | Complete type and function reference |
