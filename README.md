<p align="center">
  <img src="demo/maya_fps.gif" alt="3D FPS raycaster running in a terminal" width="700">
</p>

<h1 align="center">maya</h1>

<p align="center">
  A C++26 terminal UI framework with a compile-time DSL, flexbox layout, SIMD-accelerated rendering, and 50+ widgets.
</p>

<p align="center">
  <a href="#quickstart">Quickstart</a> · <a href="#examples">Examples</a> · <a href="#features">Features</a> · <a href="#headers">Headers</a> · <a href="#building">Building</a> · <a href="#using-maya-in-your-project">Using in your project</a>
</p>

---

## Quickstart

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

Structure, text, styles, and layout — all resolved at compile time. Pipe operators chain naturally. Type-state machines enforce correctness: you can't set a border color without a border, can't apply layout modifiers to text nodes.

A reactive counter in 12 lines:

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    Signal<int> count{0};

    run(
        {.title = "counter"},
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
}
```

For complex apps, use the **Program** architecture — pure functions, effects as data:

```cpp
struct Counter {
    struct Model { int count = 0; };
    struct Inc {}; struct Dec {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Quit>;

    static Model init() { return {}; }
    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Inc)  { return std::pair{Model{m.count + 1}, Cmd<Msg>{}}; },
            [&](Dec)  { return std::pair{Model{m.count - 1}, Cmd<Msg>{}}; },
            [](Quit)  { return std::pair{Model{}, Cmd<Msg>::quit()}; },
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

Two APIs, same runtime. Use `run(event_fn, render_fn)` for quick tools, `run<P>()` when you want testable pure logic and algebraic effects.

## Examples

21 examples ship with the framework. Here are a few:

<table>
<tr>
<td align="center"><b>Cyberpunk Dashboard</b><br><sub>Oscilloscope · Radar · Hex waterfall · Spirograph</sub></td>
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

**Also included:** matrix rain, mandelbrot zoom, fluid simulation, game of life, breakout, snake, particle systems, raymarcher, sorting visualizations, spectrum analyzer, space shooter, music player, system monitor, and more.

## Features

**DSL & Layout**
- Compile-time UI tree construction with `|` pipe operators
- Type-state safety — invalid compositions are compile errors
- Flexbox layout powered by Yoga
- Runtime text via `text()`, compile-time text via `t<"...">`

**Rendering**
- Double-buffered with dirty-region tracking
- SIMD-accelerated diff (AVX2/SSE4.2/NEON) — only changed cells hit the terminal
- 64-bit packed cells for O(1) comparison
- Cache-line aligned buffers, style interning via open-addressing hash map

**Widgets** (50+)
Line chart · Bar chart · Gauge · Sparkline · Heatmap · Progress bar · Table · Tabs · Tree view · Scrollable · Input · Textarea · Select · Slider · Checkbox · Radio · Button · Modal · Popup · Toast · Markdown · Diff view · Calendar · Breadcrumb · Spinner · Badge · Menu · Command palette · Log viewer · Canvas · Gradient · Image · and more

**Architecture**
- Elm-style Program concept: Model + Msg + init/update/view/subscribe
- Effects as data: `Cmd<Msg>` (quit, batch, after, task) and `Sub<Msg>` (keys, mouse, timers)
- Type-state render pipeline (Idle→Cleared→Painted→Opened→Closed)
- Signal/slot reactivity system (SolidJS-inspired)
- Fullscreen and inline rendering modes
- Keyboard, mouse, resize, focus/blur, paste events

## Headers

maya has a clean separation between its public API and internal implementation.

### Public API — `<maya/maya.hpp>`

The main header. Includes the DSL, app lifecycle, events, signals, styles, and element types. Does **not** include rendering internals.

```cpp
#include <maya/maya.hpp>
```

### Widgets — `<maya/widget/*.hpp>`

Widgets are included individually, not bundled into `maya.hpp`. Include only what you use:

```cpp
#include <maya/widget/markdown.hpp>
#include <maya/widget/input.hpp>
#include <maya/widget/scrollable.hpp>
#include <maya/widget/tool_call.hpp>
// ... see include/maya/widget/ for the full list
```

### Internal — `<maya/internal.hpp>`

For advanced use cases that need direct access to the canvas, diff engine, SIMD, terminal I/O, or layout engine. Most projects should never include this.

```cpp
#include <maya/internal.hpp>  // canvas_run(), Canvas, StylePool, etc.
```

### What's in each layer

| Header | Contains | Stability |
|--------|----------|-----------|
| `maya.hpp` | DSL, `run<P>()`, Program concept, Cmd, Sub, events, signals, styles, elements, themes | Stable — safe to depend on |
| `widget/*.hpp` | 50+ widgets (Input, Markdown, ToolCall, ...) | Stable — include what you need |
| `internal.hpp` | Canvas, diff, renderer, SIMD, terminal I/O, layout | Internal — may change across versions |

## Building

Requires a C++26 compiler. **GCC 15+** is recommended across all platforms.

### macOS (ARM / Intel)

AppleClang does not support C++26. Use Homebrew GCC:

```bash
brew install gcc@15 cmake
cmake -B build -DCMAKE_CXX_COMPILER=g++-15
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Linux (x86_64 / ARM64)

```bash
# Ubuntu/Debian
sudo apt install g++-15 cmake
cmake -B build -DCMAKE_CXX_COMPILER=g++-15
cmake --build build -j$(nproc)

# Arch
sudo pacman -S gcc cmake
cmake -B build
cmake --build build -j$(nproc)

# Fedora
sudo dnf install gcc-c++ cmake
cmake -B build
cmake --build build -j$(nproc)
```

### Windows

**MSYS2 (recommended):**
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
cmake -B build -G "MinGW Makefiles"
cmake --build build -j%NUMBER_OF_PROCESSORS%
```

**Visual Studio 2025+ (MSVC):**
```bash
cmake -B build -G "Visual Studio 17 2025"
cmake --build build --config Release
```

**WSL2:** Follow the Linux instructions above.

### Tests

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-15 -DMAYA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Using maya in your project

Install maya, then use `find_package` in your CMakeLists.txt:

```bash
# Install to /usr/local (or any prefix)
cmake --install build --prefix /usr/local
```

```cmake
# Your project's CMakeLists.txt
find_package(maya 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE maya::maya)
```

```cpp
// Your code
#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/input.hpp>

using namespace maya::dsl;
```

Maya uses [SameMajorVersion](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html) compatibility — any 0.x release works with `find_package(maya 0.1)`.

## License

MIT
