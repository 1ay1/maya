# maya's design: Ink to jaal's React

jaal is the runtime: a program's model, its messages, `update`, effects
(`Cmd`) and subscriptions (`Sub`), threads, timers, streams, shutdown.
maya is what draws it in a terminal: elements, layout, style, widgets,
the renderer, the terminal device, and **one** host that plugs the device
into jaal. The split is the same as React and Ink, and the rules follow
from it.

## The rules

1. **One loop, and it is jaal's.** maya has no event loop, no timers, no
   worker threads, no effect types. `maya::run<P>()` is a thin function
   that opens the terminal and hands it to `jaal::run`.
2. **A program is a jaal program with a `view()` that returns an
   `Element`.** Nothing else. There is no callback loop, no canvas loop,
   no "live" loop: a canvas demo is a program whose view is `pixels(img)`
   or `glyphs(g)`; an inline progress bar is a program run with
   `.mode = Mode::Inline`.
3. **Everything the model says, the view says.** No globals, no statics
   written by `view()`, no state hidden in widgets that the model can't
   see. Animation is a `Sub::every` in `subscribe()`, which is also where
   it stops: a still screen costs nothing.
4. **Say what you use.** The rows of `Cmd` and `Sub` list the terminal
   effects and event sources a program needs, so a host that can't provide
   one is a compile error, not a runtime surprise.
5. **Static output needs no runtime.** `maya::print(el)` and
   `render_to_string(el)` stay: they are Ink's `renderToString`, pure view.

## The layers

| Header | What | Knows jaal? |
|---|---|---|
| `<maya/maya.hpp>` | elements, DSL, layout, style, widgets, `Image`/`pixels`, `print` | no |
| `<maya/screen.hpp>` | `Screen`: the terminal device (raw mode, input, frame diff, flow control) | no |
| `<maya/host/run.hpp>` | `run<P>`, and the whole of maya with it: the include an app uses | yes |
| `include/maya/host/` | where maya meets a runtime, and the ONLY place in maya that may name jaal | yes |

`maya` (the library) stays free of jaal. `maya::app` (CMake target) is
maya + jaal, and is what a program links.

| The seam (`include/maya/host/`) | |
|---|---|
| `interop.hpp` | maya's value types, as jaal sees them (`Sendable`/`Frozen`) |
| `effects.hpp` | what only the TERMINAL can do: `set_title`, `commit_scrollback`, `suspend`, ... |
| `sources.hpp` | what the terminal REPORTS: `on_key`, `on_mouse`, ...; `Program`; `keys<Sub>()` |
| `device.hpp` | `Device`: the screen surface the host drives, as a concept |
| `terminal.hpp` | `terminal_host`: maya as a jaal host |
| `run.hpp` | `run<P>()`: open the Screen, hand it to jaal — and the app's one include |

`terminal_host<P, Dev = Screen>` is templated on its device. Production
instantiates it with `Screen`, so `terminal_host<P>` is spelled the same as
before and there is no indirection — but the host's scheduling logic (frame
debt, the animation deadline, the ack window, input held across a navigation
key) is the one part of maya that used to be reachable only through a real
pty. `tests/test_host.cpp` drives it against a fake device in microseconds;
`Device` is the contract both share, so the fake can't drift from `Screen`
without failing the `static_assert` in `device.hpp`.

The rule is mechanical, and `tests/seam.sh` runs it in CI:

    grep -rl jaal include/maya --include=*.hpp | grep -v maya/host/

prints nothing. Everything else may NAME jaal in a comment — that is how a
reader finds the seam — but may not include it or use its types.

## One file, one job

The device and the renderer are split by concern, not by size: a file is
the unit you read to understand one thing.

| The device (`include/maya/device/`, `src/device/`) | |
|---|---|
| `options.hpp` | how to take the terminal: `Mode`, `RenderBackend`, `Options` |
| `frame_request.hpp` | a widget asks for the next frame; the host schedules it |
| `keys.hpp` | key predicates, and which keys are navigation |
| `internals.hpp` | `detail::Device`: the device's state |
| `theme_canvas.hpp` | a theme's background becomes pixels |
| `create.cpp` | raw mode, alt screen or inline region, capability probes |
| `input.cpp` | resize; bytes → events |
| `render.cpp` | one frame: width check, theme edge, pick a path |
| `render_inline.cpp` | the inline path (wire gate, canvas prepare, compose) |
| `render_fullscreen.cpp` | the alt-screen path |
| `render_grid.cpp` | the Grid backend, and the off-wire warmup |
| `host_effects.cpp` | title, clipboard, raw sequences, suspend |
| `lifetime.cpp` | finalize, cleanup, destructor, moves |

| The renderer (`src/render/`) | |
|---|---|
| `renderer.cpp` | `render_tree`; `paint_element` dispatches per element kind |
| `layout_build.cpp` | element tree → layout nodes |
| `paint_box/text/list/component.cpp` | one painter per element kind |
| `paint_border.cpp` | borders and their titles |
| `serialize.cpp` | a whole frame's cells → VT bytes |
| `compose_inline.cpp` | the inline row-diff producer, and caret placement |
| `inline_state.cpp` | the inline frame's state and its scrollback proofs |

`render_internal.hpp` / `serialize_internal.hpp` / `src/device/internal.hpp`
are private to those directories: they hold what the split files share
(the component cache, the cell-run emitter, the opt-in diagnostics).

## The app API

```cpp
#include <maya/host/run.hpp>

struct Counter {
    struct Model { int n = 0; };
    struct Inc {}; struct Quit {};
    using Msg = std::variant<Inc, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, maya::on_key>;

    static Cmd update(Model& m, Inc)  { ++m.n; return {}; }
    static Cmd update(Model&,   Quit) { return Cmd::quit(0); }
    static maya::Element view(const Model& m) { return maya::dsl::text(std::to_string(m.n)); }
    static Sub subscribe(const Model&) {
        return maya::keys<Sub>({{'+', Inc{}}, {'q', Quit{}}});
    }
};

int main() { return maya::run<Counter>({.title = "counter"}); }
```

| Name | Was | Why |
|---|---|---|
| `maya::run<P>(Options)` | `run_jaal<P>(RunConfig)` | there is one run; it needs no qualifier |
| `maya::Options` | `RunConfig` | configures the terminal, not a loop |
| `maya::Program<P>` | `JaalView<P>` | says what it is, not how it's hosted (not `App`: that's what programs are called) |
| `maya::keys<Sub>({...})` | `jaal_key_map<Sub>` | reads as what it does |
| `maya::on_key`, `on_mouse`, `on_paste`, `on_focus`, `on_resize` | same | the host's event sources |
| `maya::set_title`, `commit_scrollback`, `write_clipboard`, ... | same | the host's effects |
| `maya::terminal_host<P>` | `jaal_host<P>` | the host, for tests and custom drivers |
| `detail::Device` | `detail::Runtime` | it is the terminal device; the runtime is jaal's, and only one thing may wear that name |

`jaal::` stays visible: `jaal::Cmd`, `jaal::Sub`, `Cmd::quit`,
`Sub::every` are the runtime's words, the way `useState` is React's word
in an Ink app. maya does not re-export or wrap them.

## What is deleted

- `run<P>(RunConfig)`, `run(cfg, event_fn, render_fn)`, `canvas_run`,
  `live()`: the four loops.
- `maya::Cmd`, `maya::Sub`, `detail::BackgroundQueue`, `key_map<Msg>`,
  the `Program` concept, `Ctx`, `CmdContext`: the old runtime's vocabulary.
- `maya/host/canvas.hpp`: the adapter that ran canvas callbacks on jaal.
- the `jaal_` prefix on examples: there is only one kind now.
- the Screen forwarding KEYS to scroll views: keys are the program's
  (a message and `ScrollState::handle` in update); the device still
  forwards the mouse, because only it knows where each bar was painted.

## What stays, and why

- The `Event`-variant predicates in `device/events.hpp` (`key(ev, 'q')`,
  `mouse_clicked(ev)`, ...) remain: widgets (`Input`, `List`, `Menu`, ...)
  take an `Event` in their `handle()` so one widget API serves every
  source. A program forwards `Key{k}` to a widget as `widget.handle(k)`.
- `request_animation_frame()`: a widget asks for the next frame while it
  animates (a spinner, a caret blink); the host schedules it. It is a
  per-frame request, not a loop.

## Build

C++26 is required (jaal needs structured-binding packs). `MAYA_WITH_JAAL`
is gone: the app layer builds by default (`MAYA_BUILD_APP=ON`); with it
off you get the view layer alone.

## Checking

`sh tests/smoke_all.sh build` runs every example in a real pty (54/54);
`ctest` runs the library's unit tests.
