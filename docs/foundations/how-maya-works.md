# How maya Works

The previous pages built up the problem from the ground:

- A terminal is a **grid of cells**, driven by a **byte stream**.
- **ANSI escape codes** move the cursor and style cells.
- **Input** arrives as a messy, stateful stream of bytes.
- **The rendering problem** is: redraw smoothly without flicker, tearing, or
  flooding the wire — which means *diffing* and writing only what changed.

maya is the machine that solves all of that for you. This page is the map: it
shows how the pieces fit together so the rest of the manual makes sense. Every
box here has a dedicated chapter — follow the links when you want the detail.

## The one-sentence version

> You **describe** what the UI should look like; maya **lays it out** into a
> grid of cells, **diffs** that grid against the last frame, and writes the
> **minimum** escape sequences needed to make the terminal match — atomically,
> and safely.

## The pipeline

```mermaid
flowchart TD
    DSL["<b>Your code</b><br/>the DSL: v(...), t&lt;\"...\"&gt;, pad&lt;1&gt;, | border"]
    ET["<b>Element tree</b><br/>boxes · text · widgets · style"]
    CV["<b>Canvas</b><br/>width × height grid of packed cells<br/>(glyph + style id)"]
    DF{"<b>Diff</b> vs previous Canvas<br/>(SIMD — many cells / instruction)"}
    SR["<b>Serializer</b><br/>minimal escape sequences for changed cells,<br/>wrapped in synchronized output"]
    TERM[["🖥️ Terminal grid"]]

    DSL -- build --> ET
    ET -- "layout (Yoga flexbox)" --> CV
    CV --> DF
    DF -- "only changed runs" --> SR
    SR -- bytes --> TERM
    TERM -- "stdin bytes" --> IP["<b>Input parser</b>"]
    IP -- "Event: Key / Mouse / Paste / Resize" --> DSL

    classDef you fill:#6c4cd6,stroke:#a07cff,color:#fff;
    classDef maya fill:#2a2440,stroke:#8a6cf0,color:#eee;
    class DSL you;
    class ET,CV,DF,SR,IP maya;
```

Read it as a loop: **input comes back in at the bottom**, your code reacts, the
view is rebuilt, and the **new frame goes out the top** — but only the *changed*
cells actually hit the wire.

### 1. You describe the UI — the DSL

You don't move the cursor or emit escape codes. You build an **Element tree**
with a declarative, **compile-time** DSL:

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

auto ui = (v(                                  // vertical stack
    t<"Hello">.fg(Color::Sky) | Bold,          // styled text
    hr(),                                       // a rule
    t<"Press q to quit"> | Dim
) | pad<1> | border(Rounded)).build();          // padding + a border
```

maya's DSL leans hard on C++26 template metaprogramming and **type-state
machines**, which is where the project's motto comes from:

!!! note "Impossible states don't compile"
    You can't set a border *color* before you've declared a border *style* —
    the type of the half-built element doesn't have that method yet. Negative
    padding won't compile. Modifiers only compose where they make sense. Whole
    categories of bugs are caught by the compiler instead of showing up as a
    garbled frame at runtime.

→ Full chapter: **[The Compile-Time DSL](../02-dsl.md)** and
**[Styling](../03-styling.md)**.

### 2. maya lays it out — Yoga flexbox

An Element tree says *what* you want ("a column, with a bordered box that grows
to fill, and a status line pinned at the bottom"). It does **not** say where
each cell goes. That's **layout's** job.

maya uses **Yoga**, the same battle-tested flexbox engine that powers React
Native. You get `row`/`column` stacks, `grow`/`shrink`, alignment, gaps,
padding, and responsive sizing — and maya measures text in **cells** (handling
the wide-character and grapheme problems from
[The Cell Grid](the-cell-grid.md)) so columns actually line up.

→ Full chapter: **[Layout](../04-layout.md)**.

### 3. maya paints — the Canvas

Layout produces concrete positions and sizes, and maya paints the Element tree
into a **Canvas**: a `width × height` array of **packed cells**. Each cell is a
compact 64-bit value holding a glyph plus a style id (the foreground/background/
attributes from [ANSI Escape Codes](ansi-escape-codes.md), interned so repeated
styles are cheap).

The Canvas is just data in memory — nothing has touched the terminal yet.

→ Full chapter: **[Canvas API](../08-canvas-api.md)**.

### 4. maya diffs — the part that makes it fast

This is the heart of [The Rendering Problem](the-rendering-problem.md). maya
keeps the **previous frame's** Canvas and compares it to the new one. The
comparison is **SIMD-accelerated** — it scans many cells per instruction to
find the runs that changed.

For everything that *didn't* change, maya emits **nothing**. For what did, it
emits the smallest sequence that fixes it: move the cursor there, set the style
if it differs, write the new glyphs. A clock whose seconds tick over sends a
handful of bytes per frame, not a whole screen.

!!! tip "Bytes-on-wire is the metric"
    On a real terminal — and especially over SSH — the cost of a frame is
    basically the number of bytes you write. maya's diff is typically **~10×**
    fewer bytes than re-emitting the frame, which is a structural win no
    "clear and reprint" loop can match. The whole frame is then wrapped in
    **synchronized output** (DEC 2026) so the terminal paints it atomically —
    no tearing.

→ Full chapter: **[Rendering Modes](../07-rendering-modes.md)**, and the deep
dives under **Internals**.

### 5. Input comes back in — events

While all that draws, maya reads stdin in **raw mode** and runs the byte stream
through a parser that resolves the ambiguities from
[Keyboard & Mouse Input](keyboard-and-mouse.md) — lone `Esc` vs an arrow-key
sequence, mouse reports, bracketed paste, split reads — into clean typed
**`Event`** values (`KeyEvent`, `MouseEvent`, `PasteEvent`, resize). Your code
reacts to those, the view is rebuilt, and the loop closes.

→ Full chapter: **[Event Handling](../06-events.md)**.

## Two ways to build an app

maya gives you two front-ends over the same pipeline. Pick by app size.

### A) Immediate mode — `run()` + signals

For small/medium apps: a view lambda that returns the current UI, an event
lambda that handles keys, and **[Signals](../09-signals.md)** for reactive
state. When a signal changes, the parts that depend on it re-render; the diff
handles the rest.

```cpp
Signal<int> n{0};
run(
    [&](const Event& ev) {                       // handle input
        on(ev, '+', [&]{ n.update([](int& x){ ++x; }); });
        return !key(ev, 'q');                     // false → quit
    },
    [&]{                                          // build the view
        return v(
            dyn([&]{ return text("Count: " + std::to_string(n.get())); }),
            t<"[+] increment  [q] quit"> | Dim
        ).build();
    }
);
```

### B) The Program architecture — Elm-style MVU

For larger apps: a **Model–View–Update** structure of *pure functions* —
`init` produces state, `update(state, msg)` returns new state, `view(state)`
renders it. Pure functions are easy to test and reason about, and side effects
are described as commands maya runs for you. This is the same architecture as
Elm and The Elm Architecture.

→ Full chapter: **[Runtime Content](../05-runtime-content.md)** (and the
`Program` examples).

## Full-screen vs inline rendering

maya can drive the terminal two ways:

| Mode | What it does | When to use |
|------|--------------|-------------|
| **Full-screen** | Switches to the alternate screen buffer (a clean canvas), restores your shell scrollback on exit | Dashboards, editors, games — anything that owns the whole window |
| **Inline** | Draws a live frame **at the cursor**, below your prompt, and grows/shrinks in place — your scrollback stays intact | Progress UIs, REPL-like tools, agent chat that lives in your normal terminal flow |

Inline mode is genuinely hard (it shares the screen with your shell's
scrollback, so a careless redraw can corrupt history). maya's renderer is
built around getting this right — see the **Internals** chapters
[Inline redraw paths](../internals/inline-redraw-paths.md),
[Inline autogrow](../internals/inline-autogrow.md), and
[The Witness Chain](../internals/witness-chain.md).

## Safety: never leave the terminal broken

Because maya puts the terminal in **raw mode** (and may enable the alt screen,
mouse reporting, etc.), it installs handlers so that on **any** exit route —
normal return, exception, or a fatal signal like `SIGINT`/`SIGSEGV` — it
restores cooked mode and emits the reset escapes. Your shell is always handed
back in a sane state, even after a crash. (This was a recurring theme on the
[Keyboard & Mouse Input](keyboard-and-mouse.md) page: enabling raw mode is a
promise to restore it.)

## Widgets

On top of the primitives, maya ships a large library of ready-made
**widgets** — charts, sparklines, tables, scroll views, markdown rendering,
syntax highlighting, agent-UI components, and more. They're just Elements, so
they compose with the DSL like anything else.

→ Full chapter: **[Widget Reference](../13-widgets.md)**.

## Where to go next

You now have the whole mental model. Pick a path:

<div class="grid cards" markdown>

-   :material-rocket-launch: __Build something now__

    ---

    [Getting Started](../01-getting-started.md) — install, compile, and run your
    first interactive app.

-   :material-code-tags: __Learn the DSL properly__

    ---

    [The Compile-Time DSL](../02-dsl.md) — elements, the type-state builder, and
    composition.

-   :material-application: __See it in real programs__

    ---

    [Examples Walkthrough](../10-examples.md) — dashboards, games, editors,
    explained line by line.

-   :material-cog-outline: __Go under the hood__

    ---

    The [Internals](../internals/render-roadmap.md) chapters — the diff, inline
    redraw paths, and the Witness Chain.

</div>

Welcome to maya. You came in not knowing what a terminal cell was; you leave
knowing exactly how a modern TUI is drawn — and you're ready to build one.
