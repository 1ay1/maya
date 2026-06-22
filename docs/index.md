<div class="maya-hero" markdown>

# maya

<p class="maya-tagline">
A <strong>C++26 TUI framework</strong> — a compile-time type-safe DSL, a Yoga
flexbox layout engine, reactive signals, an Elm-style runtime, and a
SIMD-accelerated cell-diff renderer. Build fast, beautiful terminal UIs with
minimal ceremony.
</p>

<p class="maya-badges">
<img alt="C++26" src="https://img.shields.io/badge/C%2B%2B-26-blue?logo=cplusplus">
<img alt="license" src="https://img.shields.io/badge/license-MIT-green">
<img alt="docs" src="https://img.shields.io/badge/docs-live-6c4cd6">
</p>

</div>

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

That is a complete, interactive terminal app. Press ++plus++ to count, ++q++ to quit.

!!! abstract "Who this manual is for"
    **Everyone — including people who have never built a TUI.** This site is a
    path from *“what even is a terminal?”* to *“I understand maya’s renderer
    internals.”* You don’t need prior terminal-UI experience; you only need to
    read C++.

## Choose your on-ramp

<div class="grid cards" markdown>

-   :material-school: __New to terminals & TUIs?__

    ---

    Start with the **Foundations** primer — five chapters that teach you
    terminals, cells, ANSI codes, input, and rendering *from absolute zero*,
    then connect it all to maya.

    [:octicons-arrow-right-24: Begin Foundations](foundations/what-is-a-terminal.md)

-   :material-rocket-launch: __Already know your way around?__

    ---

    Jump straight into building: install, compile, and ship your first
    interactive maya app in minutes.

    [:octicons-arrow-right-24: Getting Started](01-getting-started.md)

</div>

## The learning path

### :material-cube-outline: Foundations — terminals & TUIs from zero

A self-contained primer. By the end you understand how *every* TUI works.

<div class="grid cards" markdown>

-   __1 · [What Is a Terminal?](foundations/what-is-a-terminal.md)__

    ---

    The grid, the byte stream, emulators vs shells vs TTYs/PTYs — and the
    history that still shapes today’s APIs.

-   __2 · [The Cell Grid](foundations/the-cell-grid.md)__

    ---

    Cells, monospace fonts, and the surprisingly deep problem of character
    width (CJK, emoji, graphemes).

-   __3 · [ANSI Escape Codes](foundations/ansi-escape-codes.md)__

    ---

    The secret language that moves the cursor and styles text — 16/256/truecolor,
    alt screen, synchronized output.

-   __4 · [Keyboard & Mouse Input](foundations/keyboard-and-mouse.md)__

    ---

    Raw mode, keys-as-bytes, the ++esc++ ambiguity, mouse reporting, and
    bracketed paste.

-   __5 · [The Rendering Problem](foundations/the-rendering-problem.md)__

    ---

    Flicker, tearing, bytes-on-wire, and why **diffing** is the whole game.

-   __6 · [How maya Works](foundations/how-maya-works.md)__

    ---

    The map: DSL → Yoga layout → Canvas → SIMD diff → minimal escapes. It all
    comes together here.

</div>

### :material-book-open-variant: The Guide — building with maya

<div class="grid cards" markdown>

-   __[Introduction](00-introduction.md)__ — philosophy and a tour.
-   __[Getting Started](01-getting-started.md)__ — install, build, first app.
-   __[The Compile-Time DSL](02-dsl.md)__ — elements & the type-state builder.
-   __[Styling](03-styling.md)__ — colors, attributes, modifiers.
-   __[Layout](04-layout.md)__ — Yoga flexbox: rows, columns, grow/shrink.
-   __[Runtime Content](05-runtime-content.md)__ — dynamic views & the MVU Program.
-   __[Event Handling](06-events.md)__ — keys, mouse, paste, resize.
-   __[Signals & Reactivity](09-signals.md)__ — reactive state, no ceremony.
-   __[Rendering Modes](07-rendering-modes.md)__ — full-screen vs inline, the diff.
-   __[Canvas API](08-canvas-api.md)__ — the cell grid up close.
-   __[Widget Reference](13-widgets.md)__ — charts, tables, scroll, markdown, agent UI.
-   __[Examples Walkthrough](10-examples.md)__ — real programs, explained.

</div>

### :material-bookshelf: Reference & :material-flask: Internals

<div class="grid cards" markdown>

-   :material-api: __Reference__

    ---

    - [API Reference](11-api-reference.md) — every public symbol
    - [Glossary](glossary.md) — every term in one place

-   :material-cog-outline: __Internals__

    ---

    - [Render-path roadmap](internals/render-roadmap.md)
    - [Inline redraw paths](internals/inline-redraw-paths.md)
    - [Inline autogrow & ghosting](internals/inline-autogrow.md)
    - [The Witness Chain](internals/witness-chain.md)
    - [Scrollback corruption audit](internals/scrollback-corruption-audit.md)

</div>

## Why maya?

<div class="grid cards" markdown>

-   :material-shield-check: __Impossible states don’t compile__

    ---

    The DSL uses C++26 type-state machines — whole categories of UI bugs become
    compile errors, not garbled frames.

-   :material-flash: __Fast by construction__

    ---

    A SIMD cell-diff writes only what changed — typically **~10× fewer bytes**
    than redrawing. Smooth at 60fps, snappy over SSH.

-   :material-view-dashboard: __Real layout__

    ---

    Yoga flexbox — the same engine behind React Native — for rows, columns,
    grow/shrink, alignment, and gaps.

-   :material-sync: __Elm-style runtime__

    ---

    Pure `update`/`view` functions you can actually test, with side effects as
    commands.

-   :material-toy-brick: __Batteries included__

    ---

    A deep widget library, full-screen *and* inline rendering, mouse, truecolor
    with graceful downgrade.

-   :material-shield-account: __Always restores the terminal__

    ---

    Raw mode, alt screen, mouse — all cleanly restored on exit, exceptions, and
    even fatal signals.

</div>

!!! tip "Built in production"
    [**moha**](https://github.com/1ay1/moha) — a native terminal client for
    Claude — is built on maya. There are first-class Python bindings too:
    [**maya-py**](https://github.com/1ay1/maya-py).

---

→ **Ready?** Begin with [Foundations](foundations/what-is-a-terminal.md) or jump
straight to [Getting Started](01-getting-started.md).
