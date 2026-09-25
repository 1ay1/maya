# Getting Started

This is the tutorial. You will build one app and grow it step by step: a
counter, then a timer that pauses, then keys and mouse, then a pixel view,
then the same program running inline in your scrollback. Along the way you
will meet every piece of maya's single program shape and understand *every
line* of it.

Work through it top to bottom with a terminal open. It takes about 20 minutes
and leaves you able to read any maya example and write your own.

---

## 1. Install

### What you need

- **A C++26 compiler** — GCC 15+ or Clang 19+. The app layer runs on jaal,
  which needs C++26 (structured-binding packs); an older compiler will not
  build it.
- **CMake 3.28+**.
- **A Unix-like OS** — Linux or macOS. maya talks to the terminal through raw
  mode, `ioctl(TIOCGWINSZ)`, and POSIX signals.
- **The jaal submodule.** jaal is the runtime maya draws for; it lives in
  `third_party/jaal`. There is nothing else — no ncurses, no Boost, no package
  manager step.

### Build the library and examples

```bash
git clone --recursive <repo-url> maya
cd maya
git submodule update --init third_party/jaal   # if you cloned without --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

`-B build` puts all generated files in `build/` (out-of-source, so your tree
stays clean). Every file in `examples/` becomes a `maya_<name>` binary. Run a
few to confirm it works:

```bash
./build/maya_counter      # the smallest maya app — the one you'll write below
./build/maya_mandelbrot   # a pixel view, animated with a timer
./build/maya_dashboard    # a multi-panel system dashboard
```

Press `q` to quit any of them.

### Build options

| Option | Default | What it does |
|--------|---------|--------------|
| `MAYA_BUILD_APP` | `ON` | Build `maya::app` (maya on jaal). Needs C++26 and the submodule. |
| `MAYA_BUILD_EXAMPLES` | `ON` | Build the example programs (needs `maya::app`). |
| `MAYA_BUILD_TESTS` | top-level only | Build + register the test suite (`ctest`). |
| `MAYA_NATIVE_TUNING` | `ON` | Bake the host CPU in (`-march=native`). **Turn OFF for portable release builds** — a `native` binary will `SIGILL` on a chip missing the build host's instructions. |

### Use maya in your own project

Vendor maya as a subdirectory and link **`maya::app`**:

```cmake
add_subdirectory(vendor/maya)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE maya::app)   # maya + jaal + the C++26 flag
```

`maya::app` carries the include paths, jaal, and the language-standard flag.
(If you only ever call `print()` and never run a program, the plain
`maya::maya` target is enough.)

Or install it and find it:

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(maya REQUIRED)
target_link_libraries(myapp PRIVATE maya::app)
```

---

## 2. The counter

Copy `examples/counter.cpp` into your project as `main.cpp`. Here it is:

```cpp
#include <maya/app.hpp>                  // (1) maya + the jaal host

#include <string>
#include <variant>

using namespace maya;                    // (2) run, Element, on_key, keys, ...
using namespace maya::dsl;               //     t<>, text, v, h, Bold, border_, ...

struct Counter {
    struct Model { int count = 0; };                       // (3)

    struct Inc {}; struct Dec {}; struct Quit {};          // (4)
    using Msg = std::variant<Inc, Dec, Quit>;

    using Cmd = jaal::Cmd<Msg>;                            // (5)
    using Sub = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Inc)  { ++m.count; return {}; }   // (6)
    static Cmd update(Model& m, Dec)  { --m.count; return {}; }
    static Cmd update(Model&,   Quit) { return Cmd::quit(0); }

    static Element view(const Model& m) {                  // (7)
        return v(
            text("Count: " + std::to_string(m.count)) | Bold | Fg<100, 200, 255>,
            t<"[+/-] change  [q] quit"> | Dim
        ) | border_<Round> | bcol<50, 55, 70> | pad<1>;
    }

    static Sub subscribe(const Model&) {                   // (8)
        return keys<Sub>({{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}});
    }
};

int main() { return run<Counter>({.title = "counter"}); }  // (9)
```

1. **`<maya/app.hpp>`** is the one header for apps. It includes
   `<maya/maya.hpp>` (elements, DSL, styles, `print`) and the jaal host.
2. **Two namespaces.** `maya::dsl` is the vocabulary for *describing* UI —
   `t<>`, `text()`, `v()`/`h()`, style tags, and the `|` modifiers. `maya` is
   everything that *runs* it — `run`, `Element`, event sources, key helpers.
3. **`Model`** holds *all* the app's state. It must be default-constructible;
   that default is the starting state.
4. **Messages** are one empty (or data-carrying) struct each, collected in a
   `std::variant`. A message is a fact: "the user pressed +".
5. **The rows.** `jaal::Cmd<Msg>` is the effect type `update` returns;
   `jaal::Sub<Msg, on_key>` is the subscription type, and its row lists every
   event source the program uses. Use a source you didn't list and it won't
   compile.
6. **`update`** has one overload per message. It mutates the model in place
   and returns a `Cmd` — `{}` for "nothing to do", `Cmd::quit(0)` to exit with
   status 0.
7. **`view`** is a pure function of the model. `t<"...">` is compile-time text;
   `text(str)` is runtime text (anything built from the model). `v(...)` stacks
   vertically, `h(...)` horizontally; `|` adds style (`Bold`, `Fg<r,g,b>`,
   `Dim`) and box decoration (`border_<Round>`, `bcol<>` border colour,
   `pad<1>`). The compiler rejects `bcol<>` without a border.
8. **`subscribe`** says what the program listens to. `keys<Sub>` maps plain
   keys to messages.
9. **`run<Counter>(Options)`** opens the terminal and hands it to jaal's loop.
   You can check the shape up front with `static_assert(Program<Counter>);`.

Build it, run it, press `+` a few times, then `q`.

---

## 3. Adding a timer

Animation in maya is a subscription: `Sub::every(period, Msg)` sends a message
on a clock. Let's make the counter count up by itself, and pause on space.

```cpp
using namespace std::chrono_literals;

struct Counter {
    struct Model { int count = 0; bool paused = false; };

    struct Inc {}; struct Dec {}; struct Tick {}; struct Pause {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Tick, Pause, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Inc)   { ++m.count; return {}; }
    static Cmd update(Model& m, Dec)   { --m.count; return {}; }
    static Cmd update(Model& m, Tick)  { ++m.count; return {}; }
    static Cmd update(Model& m, Pause) { m.paused = !m.paused; return {}; }
    static Cmd update(Model&,   Quit)  { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return v(
            text("Count: " + std::to_string(m.count)) | Bold | Fg<100, 200, 255>,
            text(m.paused ? "paused" : "running") | Dim,
            t<"[+/-] change  [space] pause  [q] quit"> | Dim
        ) | border_<Round> | bcol<50, 55, 70> | pad<1>;
    }

    static Sub subscribe(const Model& m) {
        auto k = keys<Sub>({{'+', Inc{}}, {'-', Dec{}}, {' ', Pause{}}, {'q', Quit{}}});
        if (m.paused) return k;                             // timer dropped
        return Sub::batch(std::move(k), Sub::every(250ms, Tick{}));
    }
    static bool subs_key(const Model& m) { return m.paused; }
};
```

`subscribe()` is re-evaluated as the model changes, and jaal **diffs** the
result: when `paused` flips on, the `every` subscription disappears from the
batch and the timer is stopped; flip it off and a fresh one starts. A paused
screen costs nothing — no wakeups, no redraws.

`subs_key` is an optional hint: it returns *what `subscribe()` depends on*.
jaal only re-runs `subscribe()` when that value changes — here, only when
`paused` does, not on every tick. Return `true` (a constant) when
subscriptions never change.

For a one-shot delay instead of a clock, return `Cmd::after(500ms, Msg{})`
from `update`.

---

## 4. Keys and mouse

`keys<Sub>` covers plain keys, including special ones:
`{SpecialKey::Up, Up{}}`, `{SpecialKey::Escape, Quit{}}`. For anything richer
— modifiers, data-carrying messages — use `Sub::on` with a **captureless**
lambda that returns `std::optional<Msg>`, and the predicates `key_is`,
`ctrl_is`, `alt_is`:

```cpp
struct Click { int x, y; };
// ... add Click to Msg, and on_mouse to the Sub row:
using Sub = jaal::Sub<Msg, on_key, on_mouse>;

static Sub subscribe(const Model& m) {
    return Sub::batch(
        keys<Sub>({{'+', Inc{}}, {'-', Dec{}}, {SpecialKey::Escape, Quit{}}}),
        Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> {
            if (ctrl_is(k, 'c') || key_is(k, 'q')) return Quit{};
            return std::nullopt;
        }),
        Sub::on(on_mouse{}, [](const MouseEvent& e) -> std::optional<Msg> {
            if (e.button == MouseButton::Left && e.kind == MouseEventKind::Press)
                return Click{e.x.value, e.y.value};      // 1-based cell coordinates
            return std::nullopt;
        }));
}
```

Mouse reporting is off by default; turn it on when you run:
`run<Counter>({.title = "counter", .mouse = true})`. The other sources work the
same way: `on_resize` (`ResizeEvent{width, height}`), `on_paste`
(`PasteEvent{content}`), `on_focus` — add each to the `Sub` row as you use it.

---

## 5. A pixel view

There is no separate canvas loop: a graphics demo is a program whose `view`
returns `pixels(img)`. An `Image` is an RGB buffer drawn with half blocks, so it
has **two pixels per terminal row**.

```cpp
#include <maya/element/pixels.hpp>

struct Plasma {
    struct Model { int w = 0, h = 0, t = 0; };
    struct Tick {}; struct Resize { int w, h; }; struct Quit {};
    using Msg = std::variant<Tick, Resize, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Tick)     { ++m.t; return {}; }
    static Cmd update(Model& m, Resize r) { m.w = r.w; m.h = r.h * 2; return {}; }
    static Cmd update(Model&,   Quit)     { return Cmd::quit(0); }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");                  // no size yet
        Image img(m.w, m.h);
        img.fill_rows([&](int x, int y) -> Rgb {         // rows fill in parallel
            auto c = [](int v) { return static_cast<std::uint8_t>(v & 0xFF); };
            return {c(x * 4 + m.t), c(y * 4), c(m.t * 2)};
        });
        return pixels(std::move(img));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(33ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({{'q', Quit{}}}));
    }
    static bool subs_key(const Model&) { return true; }
};
```

`img(x, y) = Rgb{r, g, b}` sets a single pixel; `fill_rows` computes the
whole image across cores. For coloured character art rather than pixels, use
`Glyphs` + `glyphs(grid)`. `examples/mandelbrot.cpp` is this pattern at full
size.

---

## 6. Inline mode

By default `run` takes over the screen (`Mode::Fullscreen`: the alternate
screen buffer, restored on exit). Pass `.mode = Mode::Inline` and the same
program renders *in the normal terminal flow* instead — scrollback above is
kept, and when the program quits its last frame stays on screen:

```cpp
int main() { return run<Counter>({.mode = Mode::Inline}); }
```

Use inline for flow-style tools — progress cards, prompts, chat and agent UIs;
fullscreen for dashboards, editors and games. `examples/inline_progress.cpp`
is a complete inline program: a `Sub::every` tick advances a bar, and `update`
returns `Cmd::quit(0)` when it reaches 100%.

Other `Options` fields: `fps` (0 = redraw only on change; >0 redraws
continuously), `hover_motion`, `theme`, `enhanced_keyboard`, `backend`.

---

## 7. Static output with `print()`

Not everything needs a program. To draw styled output once and exit — a CLI
report, a summary after an inline run — build an element and `print` it:

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    print(v(
        t<"Build finished"> | Bold | Fg<80, 220, 120>,
        h(t<"warnings: "> | Dim, text(std::to_string(3)) | Bold)
    ) | border_<Round> | pad<1>);
}
```

`print(element)` sizes to the terminal; `print(element, width)` uses a fixed
width, and `render_to_string(element, width)` returns the rendered text as a
string instead. The output is ordinary scrollback text. This path needs no runtime,
so linking `maya::maya` is enough.

> **Coming from the old API?** `run(event_fn, render_fn)`, `live()`,
> `canvas_run()`, `maya::Cmd`/`Sub`, `key_map`, and `RunConfig` are gone. Each
> is now the program shape above: `live()` is `Mode::Inline` plus
> `Sub::every`; `canvas_run()` is a view returning `pixels(img)`.

---

## 8. Where to go next

You now know the whole program shape — model, messages, `update`, `view`,
`subscribe` — plus timers, input, pixels, and both modes. The rest of the
guide goes deep on each axis:

- **[The Compile-Time DSL](02-dsl.md)** — every node and pipe: `t<>`, `v`/`h`,
  `map`, and how type-state validation rejects impossible UIs.
- **[Styling](03-styling.md)** — colours, attributes, themes, compile-time style tags.
- **[Layout](04-layout.md)** — flexbox: direction, grow/shrink, gap, alignment,
  wrapping, borders.
- **[Event Handling](06-events.md)** — every event source, `keys<Sub>`,
  `Sub::on`, and the key predicates.
- **[Rendering Modes](07-rendering-modes.md)** — fullscreen vs inline in depth,
  scrollback commits, `print()`.
- **[Widget Reference](13-widgets.md)** — the built-in widgets.
- **[Examples Walkthrough](10-examples.md)** — annotated tours of the example
  binaries you ran in §1.

Open `examples/` — every program there has the shape you just learned.
