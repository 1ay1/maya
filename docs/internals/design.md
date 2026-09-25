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
| `<maya/app.hpp>` | `run<P>`, `App` concept, event sources, terminal effects, `keys()` | yes |

`maya` (the library) stays free of jaal. `maya::app` (CMake target) is
maya + jaal, and is what a program links.

## The app API

```cpp
#include <maya/app.hpp>

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
| `maya::App<P>` | `JaalView<P>` | says what it is, not how it's hosted |
| `maya::keys<Sub>({...})` | `jaal_key_map<Sub>` | reads as what it does |
| `maya::on_key`, `on_mouse`, `on_paste`, `on_focus`, `on_resize` | same | the host's event sources |
| `maya::set_title`, `commit_scrollback`, `write_clipboard`, ... | same | the host's effects |
| `maya::terminal_host<P>` | `jaal_host<P>` | the host, for tests and custom drivers |

`jaal::` stays visible: `jaal::Cmd`, `jaal::Sub`, `Cmd::quit`,
`Sub::every` are the runtime's words, the way `useState` is React's word
in an Ink app. maya does not re-export or wrap them.

## What is deleted

- `run<P>(RunConfig)`, `run(cfg, event_fn, render_fn)`, `canvas_run`,
  `live()`: the four loops.
- `maya::Cmd`, `maya::Sub`, `detail::BackgroundQueue`, `key_map<Msg>`,
  the `Program` concept, `Ctx`, `CmdContext`: the old runtime's vocabulary.
- `maya/jaal/canvas.hpp`: the adapter that ran canvas callbacks on jaal.
- the `jaal_` prefix on examples: there is only one kind now.

## Build

C++26 is required (jaal needs structured-binding packs). `MAYA_WITH_JAAL`
is gone: the app layer always builds.
