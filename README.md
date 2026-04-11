<p align="center">
  <img src="demo/maya_fps.gif" alt="3D FPS raycaster in a terminal" width="700">
</p>

<h1 align="center">maya</h1>

<p align="center">
  C++26 terminal UI framework.<br>
  Compile-time DSL. Flexbox layout. SIMD rendering. 69 widgets. Cross-platform.
</p>

<p align="center">
  <a href="#quickstart">Quickstart</a> · <a href="#examples">Examples</a> · <a href="#features">Features</a> · <a href="docs/">Docs</a> · <a href="#building">Building</a>
</p>

---

Build terminal apps that look good and run fast. Maya gives you a type-safe DSL that catches layout mistakes at compile time, a flexbox engine for real layout, and a rendering pipeline that diffs frames with SIMD so only changed cells hit the terminal.

Ships with 69 widgets — from inputs and tables to markdown renderers, tool call cards, and everything you need to build an AI agent interface. Runs on Linux, macOS, and Windows.

## Quickstart

Static UI:

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

Interactive counter:

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

Elm architecture for complex apps:

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

Two APIs, same runtime. `run(event_fn, render_fn)` for quick tools. `run<P>()` when you want testable pure logic and algebraic effects.

## Examples

26 examples ship with the framework:

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

Also: FPS raycaster, raymarcher, fluid simulation, mandelbrot zoom, matrix rain, particle systems, sorting visualizations, spectrum analyzer, breakout, snake, space shooter, music player, system monitor, AI agent simulation, and more. All under [`examples/`](examples/).

## Features

### DSL

- Compile-time UI trees with `|` pipe operators
- Type-state safety — can't set border color without a border, can't apply layout to text
- `t<"...">` for compile-time text, `text()` for runtime
- Composes naturally: `v()`, `h()`, `text()`, `border_<>`, `pad<>`, `Fg<>`, `Bold`

### Layout

- Flexbox via Yoga — `grow()`, `gap()`, `width()`, `height()`, `align()`, `justify()`
- Fullscreen and inline rendering modes (alternate screen or native scrollback)

### Rendering

- Double-buffered with dirty-region tracking
- SIMD-accelerated frame diff (AVX2 / SSE4.2 / NEON) — only changed cells write to terminal
- 64-bit packed cells for O(1) comparison
- Cache-line aligned buffers, style interning via open-addressing hash map

### Widgets (69)

**Data:** Line chart · Bar chart · Gauge · Sparkline · Heatmap · Flame chart · Waterfall · Token stream · Context window · Git graph

**Input:** Text input · Textarea · Select · Slider · Checkbox · Radio · Button · Menu · Command palette

**Layout:** Table · Tabs · Tree · Scrollable · Modal · Popup · Toast · Disclosure · Divider · Breadcrumb

**Display:** Markdown · Inline diff · Diff view · Badge · Spinner · Progress bar · Calendar · Canvas · Image · Log viewer

**Agent UI:** Tool call · Bash tool · Read tool · Edit tool · Write tool · Fetch tool · Message · Thinking block · Streaming cursor · Activity bar · Permission prompt · Model badge · Cost tracker · Git status · File changes · System banner · Error block · Turn divider · Conversation view · Plan view · API usage

### Architecture

- Elm-style `Program` concept: `Model` + `Msg` + `init` / `update` / `view` / `subscribe`
- Effects as data: `Cmd<Msg>` (quit, batch, after, task) and `Sub<Msg>` (keys, mouse, timers)
- Type-state render pipeline (Idle → Cleared → Painted → Opened → Closed)
- Signal/slot reactivity (SolidJS-inspired)
- Keyboard, mouse, resize, focus/blur, paste events

## Headers

```cpp
#include <maya/maya.hpp>           // DSL, run<P>(), events, signals, styles — the public API
#include <maya/widget/input.hpp>   // widgets are included individually
#include <maya/internal.hpp>       // canvas, diff engine, SIMD, terminal I/O (unstable)
```

| Header | Contains | Stability |
|--------|----------|-----------|
| `maya.hpp` | DSL, Program, Cmd, Sub, events, signals, styles, elements, themes | Stable |
| `widget/*.hpp` | 69 widgets | Stable |
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

**MSYS2 (recommended):**
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

**WSL2:** Follow the Linux instructions.

### Tests

```bash
cmake -B build -DMAYA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

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

MIT
