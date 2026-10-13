# Examples Walkthrough

maya ships **54 examples** in [`examples/`](../examples/). Every one is a
jaal program run by `maya::run<P>()`: a `Model`, one `update()` per message,
a pure `view()` that returns an `Element`, and a `subscribe()` that says where
messages come from. (The two exceptions, `stat_sheet_demo` and
`editor_widget_check`, just `print()` or `render_to_string()` and exit.)

Every `examples/<name>.cpp` is built as the target `maya_<name>` (the CMake
build globs the directory):

```sh
cmake --build build --target maya_counter && ./build/maya_counter
```

This guide walks through the patterns first, then lists all 54.

## 1. counter.cpp — the smallest app

The README's quickstart, verbatim:

```cpp
#include <maya/host/run.hpp>

#include <string>
#include <variant>

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

**Key patterns**:
- **Model is all the state**, a plain default-constructible struct.
- **One struct per message**, collected in `Msg = std::variant<...>`.
- **One `update()` overload per message.** It mutates the model in place and
  returns a `Cmd`: `{}` for nothing, `Cmd::quit(0)` to exit.
- **`Cmd` and `Sub` name what the program uses.** `jaal::Sub<Msg, on_key>`
  says it listens to the keyboard; a host that can't provide a listed source
  is a compile error.
- **`keys<Sub>({...})`** maps keys to messages declaratively.
- **`view()` is pure**: `const Model&` in, `Element` out.

`basic.cpp` is the same program again, kept for side-by-side reading.

## 2. stopwatch.cpp — ticks, delays, conditional subscriptions

**Demonstrates**: `Sub::every()`, `Cmd::after()`, subscriptions that depend on
the model.

```cpp
static Cmd update(Model& m, Lap) {
    // ... record the lap, turn the flash on ...
    return Cmd::after(std::chrono::milliseconds(300), FlashOff{});
}

static Sub subscribe(const Model& m) {
    auto on_keys = keys<Sub>({{'q', Quit{}}, {' ', Toggle{}}, {'l', Lap{}}, {'r', Reset{}}});
    if (m.running)
        return Sub::batch(std::move(on_keys),
                          Sub::every(std::chrono::milliseconds(10), Tick{}));
    return on_keys;
}
```

`subscribe()` is diffed after every update: when the stopwatch stops, the
timer is torn down and a still screen costs nothing.

## 3. inline_progress.cpp — inline mode

**Demonstrates**: `Mode::Inline`, auto-quit, printing after the program ends.

An inline program renders into the terminal's scrollback instead of the alt
screen, so its last frame stays visible after exit. The progress card is just a
program whose `Tick` advances the work and quits at 100%:

```cpp
struct Progress {
    struct Model { std::chrono::milliseconds elapsed{0}, step{33}; };
    struct Tick {};
    using Msg = std::variant<Tick>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Tick) {
        m.elapsed = std::min<std::chrono::milliseconds>(m.elapsed + m.step, kDuration);
        return m.elapsed >= kDuration ? Cmd::quit(0) : Cmd{};
    }
    static Element view(const Model& m) { return progress_card(/* ... */); }
    static Sub subscribe(const Model& m) { return Sub::every(m.step, Tick{}); }
};

int main() {
    const int rc = run<Progress>({.mode = Mode::Inline});
    print(v(t<"Done"> | Bold | Fg<100, 255, 140>, blank_,
            text("All work completed successfully.") | Dim) | pad<1> | border_<Round>);
    return rc;
}
```

`chat`, `agent`, `agent_session`, `stocks` and `sysmon` run inline too.

## 4. Compile-time DSL and one-shot output

**Demonstrates**: fully constexpr UI, type-state safety, `print()`.

```cpp
constexpr auto card = v(
    t<"System Status"> | Bold | Fg<220, 220, 240>,
    t<"">,
    h(t<"CPU:"> | Dim, t<" 42%"> | Bold | Fg<80, 220, 120>),
    h(t<"Mem:"> | Dim, t<" 8.2G"> | Bold | Fg<180, 130, 255>)
) | border_<Round> | bcol<60, 65, 80> | pad<1>;

print(card);
```

The border colour (`bcol<...>`) only compiles because `border_<Round>` comes
first. `print()` and `render_to_string()` need no program at all:
`stat_sheet_demo`, `editor_widget_check` and `markup --dump` use them.

## 5. Pixels and glyphs: games, simulations, fractals

**Demonstrates**: `pixels(Image)`, `Image::fill_rows`, `glyphs(Glyphs)`,
resize messages, `Tick` animation. See [Pixels and glyphs](08-canvas-api.md).

Every animation is a normal program. The model sizes itself from `on_resize`,
`update()` advances the simulation on `Tick`, and `view()` turns the model
into a picture:

```cpp
// doom_fire.cpp
static Element view(const Model& m) {
    if (m.w == 0) return text("");              // no size yet
    return v(pixels(render(m)), status_bar(m)); // Image + a text status bar
}

static Sub subscribe(const Model&) {
    return Sub::batch(
        Sub::every(16ms, Tick{}),
        Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
            return Resize{r.width.value, r.height.value};
        }),
        keys<Sub>({{' ', ToggleSource{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}}}));
}
```

Shaders compute each pixel on every core:

```cpp
// space3d.cpp
img.fill_rows([&](int x, int y) { return finish(trace(m, x, y, m.w, m.h), x, y, m.w, m.h); });
```

Character art uses a `Glyphs` grid instead of an Image:

```cpp
// matrix.cpp
static Element view(const Model& m) {
    if (m.w == 0) return text("");
    return v(glyphs(rain(m)), status_bar(m));
}
```

Mouse input is another subscription. `fluid.cpp` stirs the fluid by drag:

```cpp
using Sub = jaal::Sub<Msg, on_key, on_mouse, on_resize>;

auto mouse = Sub::on(on_mouse{}, [](const MouseEvent& e) -> std::optional<Msg> {
    if (e.button != MouseButton::Left && e.kind != MouseEventKind::Move) return std::nullopt;
    const int x = e.x.value - 1, y = (e.y.value - 1) * 2;   // 1-based cells -> pixels
    switch (e.kind) {
        case MouseEventKind::Press:   return Press{x, y};
        case MouseEventKind::Move:    return Drag{x, y};
        case MouseEventKind::Release: return Release{};
    }
    return std::nullopt;
});
```

(Run it with `run<FluidSim>({.title = "fluid", .mouse = true})`.)

## 6. agent_session.cpp — the reference agent app

**Demonstrates**: `Sub::stream` background worker, SSE-shaped streaming, live
tool cards, the status-bar family, inline mode with zero scrollback
corruption.

A Claude-Code-style inline TUI (~2300 lines). A background stream feeds
Anthropic-shaped events into `update()`; tool widgets change while I/O is still
arriving. The worker subscription exists only while a turn is streaming:

```cpp
static Sub subscribe(const Model& m) {
    // ...
    const bool working = m.stream_phase > 0 && !m.perm_open
                      && m.phase != Phase::Idle && m.phase != Phase::Done;
    if (!working) return Sub::batch(std::move(keys), std::move(tick));
    auto worker = Sub::stream(/* ... */);
    // ...
}
```

It runs unattended through several scenarios and has a working composer for
multi-turn input. `run<App>({.title = "agent session", .fps = 30, .mode = Mode::Inline})`.

## 7. motion_showcase.cpp — animation framework

Widgets that animate read the frame clock while `view()` runs; the program
never calls a clock itself. A one-screen tour of `Motion`, springs, timelines,
staggers and the streaming typewriter. See [Animation](14-animation.md).

## 8. terminal_fx.cpp — terminal effects

A checklist program: each key fires one terminal effect (listed in the
program's `Cmd` row, e.g. `jaal::Cmd<Msg, set_title>`) and the screen shows
what came back. `--dracula` runs it with a theme that owns its background.

## The full example set

All 54, grouped by what they teach. Target: `maya_<name>`; source:
`examples/<name>.cpp`.

**Start here** — the program shape:

- `counter` — the smallest app; the README quickstart.
- `basic` — the counter again, for side-by-side reading.
- `stopwatch` — `Sub::every` ticks, `Cmd::after` delays, subscriptions that depend on the model.
- `inline_progress` — an inline progress card that quits at 100% and prints a summary.
- `navcheck` — a tiny list, used by the navigation-frame test (every row of a key-repeat burst is drawn).
- `terminal_fx` — every terminal effect the host carries, one key each.

**Widgets and markup:**

- `widgets` — a tour of the widget library.
- `markup` — the markdown engine and HTML widget; interactive viewer, or `--dump` for a one-shot coloured dump.
- `motion_showcase` — `Motion`, springs, timelines, staggers, the streaming typewriter.
- `floating` — caret-anchored floating overlays (a popup that follows a movable caret).
- `stat_sheet_demo` — a `StatSheet` printed at three widths.

**Responsive layout:**

- `adaptive` — `pick()` + `clamp()` + `fit_col()`: good at every size, on both axes.
- `grid` — a rockbottom-shaped system dashboard in two lines (`row()` of stat cards).
- `pretty` — the pretty + responsive toolkit (`gradient()`, `rainbow()`, width breakpoints, `fit_row()`); resize while it runs.
- `viewport` — the `viewport()` layout widget with real widgets, scrollable.
- `agent_stats` — a tabbed, animated agent-stats dashboard laid out with `viewport()`.

**Scrolling:**

- `scroll_2d` — two-axis scrolling over one `ScrollState`.
- `scroll_clip` — one-axis scrolling: paint all rows, the renderer clips.
- `scroll_slice` — emit only the visible rows (how log viewers and lists scroll).
- `scroll_styles` — every built-in scrollbar style preset on one screen.
- `proc_table` — a process list on `maya::Table`: selection, sorting, scrolling.

**Agent and chat UIs:**

- `agent` — a simulated coding-agent session: thinking, tool calls, streaming response (inline, 20 fps).
- `agent_session` — the reference agent app: a background stream, live tool cards, a working composer (inline).
- `chat` — a scripted AI agent session with streaming content and a live composer (inline).
- `messenger` — multi-channel terminal chat with keyboard and mouse.

**Dashboards:**

- `dashboard` — NEXUS: oscilloscope, spectrum, radar, hex waterfall and gauges on a `Glyphs` grid.
- `deploy` — a CI/CD deployment pipeline: waves, rollback, environments.
- `hacker` — a movie-style hacking terminal: scrolling data, alerts, hex dumps.
- `ide` — a VS Code / Zed-style IDE layout with a simulated build.
- `music` — a music player with animated album art, a visualiser and a playlist.
- `space` — NASA-style mission control tracking a journey to Mars.
- `stocks` — a live stock ticker with charts and a news feed (inline).
- `sysmon` — a system monitor / hacker console with fake telemetry (inline).

**Editor widgets** (see [Editor widgets](editor-widgets.md)):

- `editor_ide` — the editor widgets composed as a workspace: outline, code view with blame, panels.
- `editor_live` — a real editable buffer (`TextEditor`) with highlighting and selection.
- `editor_widgets` — widget showcase browser, one widget at a time.
- `editor_widgets2` — second showcase: git, debug, panels, decorations.
- `editor_widgets3` — third showcase: decorations, lenses, status readouts, doc rendering.
- `editor_workbench` — a full IDE shell from `Workbench`, `ActivityBar`, `SplitView`.
- `editor_widget_check` — lifetime + render smoke test: every widget as a temporary, rendered via `render_to_string`.

**Pixels and glyphs** — games, simulations, fractals, 3D:

- `doom_fire` — the PSX Doom fire: heat field in the model, palette into an Image, floating embers.
- `doomfire2` — the Doom fire again, the minimal reference for how apps are written now.
- `matrix` — Matrix digital rain on a `Glyphs` grid, with a "wake up" message.
- `life` — Conway's Game of Life on a torus, cells coloured by age.
- `fluid` — Stam's stable-fluids solver; stir it with the mouse.
- `particles` — five particle systems: fireworks, galaxy, fountain, vortex, starfield.
- `breakout` — Breakout: the board as an Image, power-ups and game-over card as elements over it (`zstack`).
- `snake` — Snake with a gradient body, pulsing food, sparks and a ghost trail.
- `sorts` — eight sorting algorithms racing side by side.
- `spectrum` — a simulated audio spectrum analyser: five synthetic tracks, 64 bands, beat detection.
- `mandelbrot` — an animated Mandelbrot zoom with `fill_rows` and six palettes.
- `fps` — a Wolfenstein-style raycaster with textured walls, enemies and a minimap.
- `raymarch` — a real-time SDF raymarcher: reflections, sunset sky, coloured lights, four scenes.
- `space3d` — flight over raymarched terrain with water, shadows and gold rings to fly through.
