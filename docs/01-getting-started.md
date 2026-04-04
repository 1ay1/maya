# Getting Started

## Requirements

- **C++26 compiler**: GCC 14+ or Clang 19+ with `-std=c++2c`
- **CMake 3.28+**
- **Unix-like OS**: Linux, macOS (terminal raw mode, `ioctl`, signal handling)
- No external dependencies — maya is self-contained

## Building

```bash
git clone <repo-url> maya
cd maya
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This builds `libmaya.a` and all 10 example programs.

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `MAYA_BUILD_EXAMPLES` | `ON` | Build the example programs |
| `MAYA_BUILD_TESTS` | `OFF` | Build and register the test suite |

```bash
# Build with tests
cmake -B build -DMAYA_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build
```

### Running Examples

```bash
./build/maya_counter      # Minimal counter (fullscreen)
./build/maya_demo         # Feature showcase (fullscreen)
./build/maya_dsl          # Compile-time DSL demo (one-shot print)
./build/maya_inline       # Inline mode counter (scrollback)
./build/maya_dashboard    # System monitoring dashboard
./build/maya_agent        # AI coding agent simulation (inline)
./build/maya_progress     # Package install progress (inline)
./build/maya_matrix       # Matrix digital rain (canvas)
./build/maya_viz          # Signal charts + heatmap (canvas)
./build/maya_fireworks    # Particle fireworks (canvas)
```

## Using maya in Your Project

### As a CMake Subdirectory

```cmake
add_subdirectory(vendor/maya)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE maya)
```

### As an Installed Library

```bash
cmake --install build --prefix /usr/local
```

Then in your project:

```cmake
find_package(maya REQUIRED)
target_link_libraries(myapp PRIVATE maya::maya)
```

## Your First App

Create `main.cpp`:

```cpp
#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;

int main() {
    run(
        [](const Event& ev) { return !key(ev, 'q'); },
        [] {
            return (v(
                t<"Hello, maya!"> | Bold | Fg<100, 180, 255>,
                t<"Press q to quit"> | Dim
            ) | border_<Round> | pad<1>).build();
        }
    );
}
```

Build and run:

```bash
cmake -B build
cmake --build build
./build/myapp
```

You'll see a bordered box with styled text. Press `q` to exit.

## The Two Namespaces

maya exposes two namespaces:

```cpp
using namespace maya;       // Framework: run(), Signal, Event, quit(), print()...
using namespace maya::dsl;  // UI building: t<>, v(), h(), Bold, Fg<>, border_<>...
```

Almost every maya program uses both. The DSL namespace contains everything
needed to construct UI trees — nodes, style tags, layout modifiers, and
factory functions. The root `maya` namespace contains the runtime framework —
the app loop, event handling, reactive signals, and rendering.

## The .build() Pattern

DSL nodes are compile-time objects. To convert them into runtime `Element`
values that the framework can render, call `.build()`:

```cpp
constexpr auto ui = v(t<"Hello">) | pad<1>;  // Compile-time node
Element elem = ui.build();                     // Runtime element
```

The `run()` render function must return an `Element`, so your render lambda
typically ends with `.build()`:

```cpp
[&] {
    return (v(
        t<"Hello"> | Bold,
        dyn([&] { return text(message); })
    ) | pad<1>).build();  // <-- .build() converts DSL tree to Element
}
```

## Next Steps

- [The DSL](02-dsl.md) — learn the compile-time UI building blocks
- [Styling](03-styling.md) — colors, text attributes, and themes
- [Layout](04-layout.md) — flexbox positioning and borders
- [Rendering Modes](07-rendering-modes.md) — choose the right mode for your app
