# maya

A **C++26 TUI framework** — a compile-time type-safe DSL, a Yoga flexbox layout
engine, reactive signals, an Elm-style runtime, and a SIMD-accelerated cell-diff
renderer. Build fast, readable terminal user interfaces with minimal ceremony.

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

## Where to start

This manual goes from *"what is a terminal?"* to *"how maya's renderer works."*
You don't need prior terminal-UI experience — just C++.

- **New to terminals or TUIs?** Begin with [Foundations](foundations/what-is-a-terminal.md)
  — five short chapters that explain terminals, cells, escape codes, input, and
  rendering from the ground up, then connect them to maya.
- **Already comfortable in a terminal?** Jump straight to
  [Getting Started](01-getting-started.md).

## Foundations

A self-contained primer on how every TUI works.

1. [What Is a Terminal?](foundations/what-is-a-terminal.md) — the grid, the byte stream, emulators vs shells vs TTYs/PTYs.
2. [The Cell Grid](foundations/the-cell-grid.md) — cells, monospace fonts, and the problem of character width.
3. [ANSI Escape Codes](foundations/ansi-escape-codes.md) — moving the cursor and styling text; 16/256/truecolor.
4. [Keyboard & Mouse Input](foundations/keyboard-and-mouse.md) — raw mode, key bytes, mouse, and paste.
5. [The Rendering Problem](foundations/the-rendering-problem.md) — flicker, tearing, and why diffing wins.
6. [How maya Works](foundations/how-maya-works.md) — the pipeline that ties it all together.

## Guide

- [Introduction](00-introduction.md) — philosophy and a tour.
- [Getting Started](01-getting-started.md) — install, build, your first UI and app.
- [The Compile-Time DSL](02-dsl.md) — elements and the type-state builder.
- [Styling](03-styling.md) — colors, attributes, modifiers.
- [Layout](04-layout.md) — Yoga flexbox: rows, columns, grow/shrink, alignment.
- [Runtime Content](05-runtime-content.md) — dynamic views and the Program (MVU) architecture.
- [Event Handling](06-events.md) — keys, mouse, paste, resize.
- [Signals & Reactivity](09-signals.md) — reactive state.
- [Rendering Modes](07-rendering-modes.md) — full-screen vs inline, the diff, synchronized output.
- [Canvas API](08-canvas-api.md) — the cell grid up close.
- [Widget Reference](13-widgets.md) — charts, tables, scroll views, markdown, agent UI.
- [Examples Walkthrough](10-examples.md) — real programs, explained.

## Reference

- [API Reference](11-api-reference.md) — every public symbol.
- [Glossary](glossary.md) — every term in one place.

## Internals

For the curious and for contributors.

- [Render-path roadmap](internals/render-roadmap.md)
- [Inline-mode redraw paths](internals/inline-redraw-paths.md)
- [Inline autogrow & the ghosting trap](internals/inline-autogrow.md)
- [The Witness Chain](internals/witness-chain.md)
- [Scrollback corruption audit](internals/scrollback-corruption-audit.md)

## Why maya?

- **Impossible states don't compile.** The DSL uses C++26 type-state machines, so
  whole categories of UI bugs become compile errors.
- **Fast by construction.** A SIMD cell-diff writes only what changed — typically
  ~10× fewer bytes than redrawing the frame. Smooth at 60fps, snappy over SSH.
- **Real layout.** Yoga flexbox, the same engine behind React Native.
- **Elm-style runtime.** Pure `update`/`view` functions you can test.
- **Batteries included.** A deep widget library, full-screen and inline rendering,
  mouse, and truecolor with graceful downgrade — and the terminal is always
  restored cleanly on exit, exceptions, and fatal signals.

[**moha**](https://github.com/1ay1/moha), a native terminal client for Claude, is
built on maya in production. First-class Python bindings live at
[**maya-py**](https://github.com/1ay1/maya-py).
