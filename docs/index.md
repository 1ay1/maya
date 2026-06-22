# maya

**A C++26 TUI framework** — a compile-time type-safe DSL, a Yoga flexbox layout
engine, reactive signals, an Elm-style runtime, and a SIMD-accelerated cell-diff
renderer. Build fast, beautiful terminal user interfaces with minimal ceremony.

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    Signal<int> n{0};
    run(
        [&](const Event& ev) {
            on(ev, '+', [&]{ n.update([](int& x){ ++x; }); });
            return !key(ev, 'q');                 // q quits
        },
        [&]{
            return (v(
                dyn([&]{ return text("Count: " + std::to_string(n.get())); }),
                t<"[+] increment   [q] quit"> | Dim
            ) | pad<1> | border(Rounded)).build();
        }
    );
}
```

That is a complete, interactive terminal app.

---

## Who this manual is for

**Everyone — including people who have never built a TUI.** This site is built
as a path from *"what even is a terminal?"* to *"I understand maya's renderer
internals."* You do not need prior terminal-UI experience; you only need to be
comfortable reading C++.

!!! tip "Two on-ramps"
    - **New to terminals/TUIs?** Start with **[Foundations](foundations/what-is-a-terminal.md)** —
      five short chapters that teach you terminals, cells, ANSI codes, input,
      and rendering *from zero*, then connect it all to maya.
    - **Know your way around a terminal?** Jump to
      **[Getting Started](01-getting-started.md)** and the Guide.

## The learning path

### 🧱 Foundations — terminals & TUIs from zero
A self-contained primer. By the end you understand how *every* TUI works.

1. [What Is a Terminal?](foundations/what-is-a-terminal.md) — the grid, the byte stream, emulators vs shells vs TTYs.
2. [The Cell Grid](foundations/the-cell-grid.md) — cells, monospace fonts, and the deep problem of character width.
3. [ANSI Escape Codes](foundations/ansi-escape-codes.md) — the language that moves the cursor and styles text.
4. [Keyboard & Mouse Input](foundations/keyboard-and-mouse.md) — raw mode and the messy reality of reading keys.
5. [The Rendering Problem](foundations/the-rendering-problem.md) — flicker, tearing, and why diffing wins.
6. [How maya Works](foundations/how-maya-works.md) — the map that ties it all to maya's architecture.

### 📘 The Guide — building with maya
- [Introduction](00-introduction.md) — philosophy and a tour.
- [Getting Started](01-getting-started.md) — install, build, your first UI and app.
- [The Compile-Time DSL](02-dsl.md) — elements, the type-state builder, composition.
- [Styling](03-styling.md) — colors, attributes, modifiers.
- [Layout](04-layout.md) — Yoga flexbox: rows, columns, grow/shrink, alignment.
- [Runtime Content](05-runtime-content.md) — dynamic views and the Program (MVU) architecture.
- [Event Handling](06-events.md) — keys, mouse, paste, resize.
- [Signals & Reactivity](09-signals.md) — reactive state without ceremony.
- [Rendering Modes](07-rendering-modes.md) — full-screen vs inline, the diff, synchronized output.
- [Canvas API](08-canvas-api.md) — the cell grid up close.
- [Widget Reference](13-widgets.md) — charts, tables, scroll views, markdown, agent UI, and more.
- [Examples Walkthrough](10-examples.md) — real programs, explained.

### 📖 Reference
- [API Reference](11-api-reference.md) — every public symbol.
- [Glossary](glossary.md) — every term in one place.

### 🔬 Internals — how the renderer really works
For the curious and for contributors.
- [Render-path roadmap](internals/render-roadmap.md)
- [Inline-mode redraw paths](internals/inline-redraw-paths.md)
- [Inline autogrow & the ghosting trap](internals/inline-autogrow.md)
- [The Witness Chain — type-theoretic scrollback integrity](internals/witness-chain.md)
- [Scrollback corruption audit](internals/scrollback-corruption-audit.md)

### 🗺️ Project
- [Roadmap](12-roadmap.md) — where maya is headed.

## Why maya?

- **Impossible states don't compile.** The DSL uses C++26 type-state machines —
  whole categories of UI bugs become compile errors.
- **Fast by construction.** A SIMD cell-diff writes only what changed, typically
  ~10× fewer bytes than re-drawing the frame. Smooth at 60fps, snappy over SSH.
- **Real layout.** Yoga flexbox — the same engine behind React Native.
- **Elm-style runtime.** Pure `update`/`view` functions you can actually test.
- **Batteries included.** A deep widget library, full-screen *and* inline
  rendering, mouse, truecolor with graceful downgrade, and a terminal that's
  always restored cleanly — even on a crash.

[**moha**](https://github.com/1ay1/moha) — a native terminal client for
Claude — is built on maya in production.

→ **Ready?** Begin with [Foundations](foundations/what-is-a-terminal.md) or jump
straight to [Getting Started](01-getting-started.md).
