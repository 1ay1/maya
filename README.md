<p align="center">
  <img src="demo/maya_fps.gif" alt="3D FPS raycaster in a terminal" width="700">
</p>

<h1 align="center">maya</h1>

<p align="center">
  C++26 TUI framework with type-state compile-time DSL, Yoga flexbox,<br>
  SIMD cell-diff renderer, and Elm-style runtime.
</p>

<p align="center">
  <a href="#quickstart">Quickstart</a> · <a href="#examples">Examples</a> · <a href="#widgets">Widgets</a> · <a href="docs/">Docs</a> · <a href="#building">Building</a>
</p>

---

- **Compile-time UI trees.** `t<"Hello"> | Bold | border_<Round>` is type-state safe — try to set border color without a border and it's a compile error, not a runtime no-op.
- **SIMD frame diff.** AVX2 / SSE4.2 / NEON. 64-bit packed cells, O(1) compare. Only changed cells write to the terminal.
- **Real flexbox.** Yoga layout — `grow()`, `gap()`, `align()`, `justify()`. No `printf` column-counting.
- **Responsive by measurement.** `fit_row` sheds low-priority items, `solve_columns` keeps a table's header and rows on one width plan, `fill` sizes a graph to its slot, `responsive` switches layouts at breakpoints, `place` pins content to any corner — all measured, never hand-estimated. See [Responsive Layouts](docs/15-responsive.md).
- **Pretty by default.** `gradient("MAYA", a, b)` sweeps color across text, `rainbow()` does the full spectrum, `gradient_rule()` draws a divider that re-tiles to its pane — one `TextElement` under the hood, so it wraps and measures like plain text. See [Gradients](docs/03-styling.md#gradients).
- **Two render modes.** Fullscreen (alternate screen) or **inline** (lives in your scrollback, doesn't take over the terminal).
- **Two app APIs.** `run(event_fn, render_fn)` for quick tools; `run<Program>()` Elm-style for testable pure logic with algebraic effects.
- **Header-mostly.** `#include <maya/maya.hpp>` is the public surface. Widgets opt-in individually.

## Quickstart

Static UI — fully resolved at compile time:

```cpp
#include <maya/maya.hpp>
using namespace maya::dsl;

int main() {
    constexpr auto ui = v(
        t<"Hello World"> | Bold | Fg<100, 180, 255>,
        h(
            t<"Status:"> | Dim,
            t<"Online"> | Bold | Fg<80, 220, 120>
        ) | border_<Round> | bcol<50, 55, 70> | pad<1>
    );
    maya::print(ui.build());
}
```

Interactive counter — quick-tool API:

```cpp
Signal<int> count{0};
run({.title = "counter"},
    [&](const Event& ev) {
        if (key(ev, '+')) count.update([](int& n) { ++n; });
        if (key(ev, '-')) count.update([](int& n) { --n; });
        return !key(ev, 'q');
    },
    [&] {
        return v(
            text("Count: " + std::to_string(count.get())) | Bold | Fg<100, 200, 255>,
            t<"[+/-] change  [q] quit"> | Dim
        ) | border_<Round> | bcol<50, 55, 70> | pad<1>;
    }
);
```

Same counter — Elm-style `Program` for testable pure logic:

```cpp
struct Counter {
    struct Model { int count = 0; };
    struct Inc {}; struct Dec {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Quit>;

    static Model init() { return {}; }
    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Inc) { return std::pair{Model{m.count + 1}, Cmd<Msg>{}}; },
            [&](Dec) { return std::pair{Model{m.count - 1}, Cmd<Msg>{}}; },
            [](Quit) { return std::pair{Model{}, Cmd<Msg>::quit()}; },
        }, msg);
    }
    static Element view(const Model& m) {
        return v(text(m.count) | Bold, t<"[+/-] q quit"> | Dim) | pad<1> | border_<Round>;
    }
    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}});
    }
};
int main() { run<Counter>({.title = "counter"}); }
```

## Examples

36 examples ship with the framework:

<table>
<tr>
<td align="center"><b>Dashboard</b><br><sub>Oscilloscope · Radar · Hex waterfall · Spirograph</sub></td>
<td align="center"><b>Stock Ticker</b><br><sub>Live charts · Sparklines · Portfolio tracking</sub></td>
</tr>
<tr>
<td><img src="demo/maya_dashboard.gif" width="400"></td>
<td><img src="demo/maya_stocks.gif" width="400"></td>
</tr>
<tr>
<td align="center"><b>Doom Fire</b><br><sub>Classic fire effect · Half-block rendering</sub></td>
<td align="center"><b>IDE</b><br><sub>Syntax highlighting · File tree · Tabs</sub></td>
</tr>
<tr>
<td><img src="demo/maya_doom_fire.gif" width="400"></td>
<td><img src="demo/maya_ide.gif" width="400"></td>
</tr>
</table>

Plus FPS raycaster, raymarcher, fluid sim, mandelbrot zoom, matrix rain, particle systems, spectrum analyzer, breakout, snake, music player, system monitor, AI agent demos, and more. All under [`examples/`](examples/).

[**moha**](https://github.com/1ay1/moha) — a native terminal client for Claude — is built on maya in production.

## Widgets

**Data:** Line chart · Bar chart · Gauge · Sparkline · Heatmap · Flame chart · Waterfall · Token stream · Context window · Git graph

**Input:** Text input · Textarea · Select · Slider · Checkbox · Radio · Button · Menu · Command palette

**Layout:** Table · Tabs · Tree · Scrollable · Modal · Popup · Toast · Disclosure · Divider · Breadcrumb

**Display:** Markdown · Inline diff · Diff view · Badge · Spinner · Progress bar · Calendar · Canvas · Image · Log viewer

**Agent UI:** Tool call · Bash tool · Read tool · Edit tool · Write tool · Fetch tool · Message · Thinking block · Streaming cursor · Activity bar · Permission prompt · Model badge · Cost tracker · Git status · File changes · System banner · Error block · Turn divider · Conversation view · Plan view · API usage

## Runtime

- Elm-style `Program` concept: `Model` + `Msg` + `init` / `update` / `view` / `subscribe`.
- Effects as data: `Cmd<Msg>` (`quit`, `batch`, `after`, `task`) and `Sub<Msg>` (keys, mouse, timers).
- Signal / slot reactivity (SolidJS-inspired).
- Type-state render pipeline: Idle → Cleared → Painted → Opened → Closed.
- Keyboard, mouse, resize, focus/blur, and bracketed-paste events out of the box.

## Headers

```cpp
#include <maya/maya.hpp>           // DSL, run<P>(), events, signals, styles — public API
#include <maya/widget/input.hpp>   // widgets included individually
#include <maya/internal.hpp>       // canvas, diff engine, SIMD, terminal I/O (unstable)
```

| Header | Contains | Stability |
|--------|----------|-----------|
| `maya.hpp` | DSL, Program, Cmd, Sub, events, signals, styles, elements, themes | Stable |
| `widget/*.hpp` | 90+ widgets | Stable |
| `internal.hpp` | Canvas, diff, renderer, SIMD, terminal I/O, layout | Internal |

## Building

Requires C++26. GCC 15+ recommended on all platforms.

### Linux

```bash
# Arch
sudo pacman -S gcc cmake
cmake -B build && cmake --build build -j$(nproc)

# Ubuntu/Debian
sudo apt install g++-15 cmake
cmake -B build -DCMAKE_CXX_COMPILER=g++-15
cmake --build build -j$(nproc)

# Fedora
sudo dnf install gcc-c++ cmake
cmake -B build && cmake --build build -j$(nproc)
```

### macOS

AppleClang doesn't support C++26. Use Homebrew GCC:

```bash
brew install gcc@15 cmake
cmake -B build -DCMAKE_CXX_COMPILER=g++-15
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Windows

**MSYS2** (recommended):
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
cmake -B build -G "MinGW Makefiles"
cmake --build build -j%NUMBER_OF_PROCESSORS%
```

**Visual Studio 2025+:**
```bash
cmake -B build -G "Visual Studio 17 2025"
cmake --build build --config Release
```

**WSL2:** follow the Linux instructions.

### Tests

```bash
cmake -B build -DMAYA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Tests link a non-LTO `-O1` `maya_test` library by default (`MAYA_FAST_TESTS=ON`)
so a header edit recompiles + relinks in seconds instead of waiting on the
production `-O3 + LTO` build. Pass `-DMAYA_FAST_TESTS=OFF` to test against the
full optimized library.

## Using maya in your project

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(maya 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE maya::maya)
```

```cpp
#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>
```

## License

MIT.
