# Migrating from maya's old runtime

maya used to ship its own event loops: `run<P>(RunConfig)` with
`update(Model, Msg) -> pair<Model, Cmd<Msg>>`, a callback
`run(event_fn, render_fn)`, `canvas_run` for animations and `live()` for
inline output. They are gone. The runtime is now
[jaal](../third_party/jaal/README.md), and there is one way to write an
app; maya is to jaal what Ink is to React. The rules behind it are in
[internals/design.md](internals/design.md).

Your `view()`, widgets, DSL and themes don't change. The model, messages,
`update`, effects and subscriptions change shape, because jaal checks at
compile time what maya checked at runtime, or never. Every example in
`examples/` is on the new API; `git log` shows each one's port next to
the original.

## At a glance

| Old | New |
|---|---|
| `#include <maya/maya.hpp>`, link `maya::maya` | `#include <maya/host/run.hpp>`, link `maya::app` |
| `run<P>(RunConfig{...})` | `run<P>(Options{...})` (same fields) |
| `static Model init()` / `init() -> pair<Model, Cmd>` | `Model{}` (default-constructed) + optional `static Cmd init(Model&)` |
| `update(Model, Msg) -> pair<Model, Cmd<Msg>>` + `std::visit(overload{...})` | one `static Cmd update(Model&, Case)` per alternative |
| `Cmd<Msg>::none()` / `Cmd<Msg>::quit()` | `{}` / `Cmd::quit(0)` |
| `Sub<Msg>`, `key_map<Msg>({...})` | `jaal::Sub<Msg, on_key, ...>`, `keys<Sub>({...})` |
| `run(cfg, event_fn, render_fn)` | a Program: state in `Model`, keys as messages, `view()` |
| `canvas_run(cfg, resize, event, paint)` | a Program whose view is `pixels(Image)` or `glyphs(Glyphs)` ([pixels](08-canvas-api.md)) |
| `live({.fps}, render)` | a Program run with `.mode = Mode::Inline` |
| `maya::quit()` | `return Cmd::quit(0);` |
| `maya::set_mouse(b)` | the `set_mouse` effect: `return Cmd(SetMouse{b});` |
| globals / function-local statics | fields of `Model` (the RNG too) |
| RNG or mutation inside `view()` | done in `update`, stored in the model |
| arrow keys auto-scrolling a `ScrollState` | route them: `Scroll{key}` → `state.handle(key, h)` in `update` (the wheel is still automatic) |
| `MAYA_WITH_JAAL`, `<maya/host/terminal.hpp>`, `run_jaal`, `JaalView`, `jaal_key_map` | gone: `<maya/host/run.hpp>`, `run`, `Program`, `keys` |

The sections below are the parts that need more than a rename.

## 2. The rows: say what you use

A jaal `Cmd` and `Sub` carry a **row**, a list of the effect and source
kinds the program may use. The core effects (`quit`, `send`, `after`,
`task`, `now`, `random`) and the core sources (`every`, `stream`) are
always included. Anything the *host* provides has to be listed:

```cpp
using Cmd = jaal::Cmd<Msg, commit_scrollback, set_title>;   // terminal effects
using Sub = jaal::Sub<Msg, on_key, on_mouse, on_resize, jaal::fx::on_signal>;
```

maya provides `on_key`, `on_mouse`, `on_paste`, `on_focus` and
`on_resize` (sources) and the terminal effects listed in the
[API reference](11-api-reference.md#effects-cmd).
A host that can't provide one (a test host, a GUI) rejects the program at
compile time, where maya would fail at runtime.

## 3. Subscriptions

| maya                          | jaal                                                         |
|-------------------------------|--------------------------------------------------------------|
| `key_map<Msg>({{'q', Quit{}}})` | `keys<Sub>({{'q', Quit{}}})` (same table, same `key_is` matching) |
| `on_key(fn)`                  | `Sub::on(on_key{}, fn)`, where `fn` returns `std::optional<Msg>` |
| `on_mouse(fn)`, `on_resize(fn)` | `Sub::on(on_mouse{}, fn)`, `Sub::on(on_resize{}, fn)`        |
| `Sub<Msg>::every(d, msg)`     | `Sub::every(d, msg)`                                         |
| `Sub::batch(a, b)`            | `Sub::batch(a, b)`                                           |
| Ctrl+C handled by maya        | `Sub::on_signal({jaal::sig::interrupt}, ...)` if you want a message for it |

`subscribe(const Model&)` is still re-evaluated after each update. If it
only depends on part of the model, you can add
`static auto subs_key(const Model&)`. jaal then skips the rebuild while
that key stays the same. A timer or stream that drops out of the returned
`Sub` gets stopped, and any messages it still had queued are dropped. That
also covers messages already in the same batch.

## 4. Effects: tasks and streams are captureless

This change touches the most code. maya's `Cmd::task` takes any callable,
lambdas with captures included. jaal's does not:

```cpp
// A task: runs once, may send any number of messages.
static void load(jaal::Sink<Msg> out, std::stop_token st, std::string path) {
    if (st.stop_requested()) return;
    out.send(Loaded{read_file(path)});
}
return Cmd::task(load, m.path);              // args are copied, must be Sendable

// A stream: a keyed, long-running source; lives while subscribe() returns it.
return Sub::stream("turn/" + std::to_string(m.turn), turn_worker, m.prompt);
```

The body is a plain function (or a captureless lambda), and anything it
needs goes in as an argument. Each argument has to satisfy
`jaal::Sendable`, meaning it is owned and has no shared mutable state.
`std::string`, value structs and vectors of those all pass.
`std::string_view`, raw pointers, `shared_ptr<T>` to mutable `T` and
atomics don't. For a type of your own, opt in explicitly:

```cpp
template <> inline constexpr bool jaal::sendable_opt_in<MyPod> = true;
```

(The host already opts in `maya::Strong<Tag,T>`, which covers `Columns`,
`Rows` and so the sizes in every event, as well as `ScrollbackDebt`.)

Every task gets a `std::stop_token`. When it can't finish on its own, it
should check the token or the return value of `out.send(...)`, which comes
back `false` once the loop is gone. At shutdown jaal gives tasks a grace
period (`run_options::kernel.shutdown_grace`, 2 s by default: the second
argument of `run`) and then detaches
the ones still running. A task that ignores its
token therefore can't hang quit.

Use `Cmd::task(jaal::placement::isolated, ...)` for work that might block
indefinitely (a stuck syscall). It gets its own thread instead of a pool
slot.

## 5. The pattern Sendable forces: decisions live in the model

In maya you can let a worker block until the UI decides something:

```cpp
// maya: the worker polls a flag the UI thread flips
auto gate = std::make_shared<PermSync>();          // atomic<bool> inside
Cmd<Msg>::task([gate](auto dispatch) {
    dispatch(PermissionAsk{});
    while (!gate->granted) sleep(10ms);             // one thread waits on another
    run_tool(dispatch);
});
```

jaal rejects this, because `shared_ptr<PermSync>` is not Sendable. The
port (`examples/agent_session.cpp`) splits the work **at the gate**:

1. Phase 1 streams until it reaches the permission point, sends
   `PermissionAsk` and returns.
2. `update(Model&, PermissionAsk)` records `perm_open = true`, which the
   view shows.
3. `update(Model&, GrantPerm)` sets `stream_phase = 2`.
4. `subscribe()` keys the worker on `(turn, phase)`. When the key changes,
   jaal stops the old stream and starts phase 2.

```cpp
auto worker = Sub::stream(
    "turn/" + std::to_string(m.turn_number) + "/" + std::to_string(m.stream_phase),
    turn_worker, m.turn_prompt, m.stream_phase);
```

No thread waits on another, and the whole decision is in the model, so it
shows up in replays and tests. A denial is the model simply not advancing
the phase. Any "worker waits for the UI" flow maps onto this.

## 6. Timing: fps, animation frames, debounce

- `Options{.fps = N}` works as before: jaal renders continuously at N
  frames per second, keeps phase and resyncs after a stall instead of
  firing catch-up frames.
- Widgets that call `request_animation_frame()` during `view()` get
  scheduled frames without you doing anything. The host redraws them with
  no input and never spins idle.
- To debounce input, keep a `jaal::debounce<T>` in the model
  (`<jaal/core/debounce.hpp>`) and fire it from a `Sub::every` or
  `Cmd::after`. It's a plain value, not an effect.

## 7. Composition

- Child programs: `jaal::child<...>` embeds one. `jaal::children<Child,
  ParentMsg, Wrap>` embeds a keyed list, where `Wrap` is
  `struct { Id id; Child::Msg msg; }`. Streams in two children can't
  collide, because each child's keys get its id as a prefix.
- Lifting a child's `Cmd`/`Sub`: `map(f)` needs a captureless `f`, because
  it may run on the worker thread. To tag with an id, use
  `map_with(id, f)`, which carries the (Sendable) id by value.
- Large apps: `Msg` may be a `std::variant` **of variants** (domain groups).
  Handle a whole group with one `update(Model&, Group)`, or leave it out
  and handle each leaf. You can put each group in its own TU.

## 8. Terminal effects

```cpp
using Cmd = jaal::Cmd<Msg, commit_scrollback, set_title>;
return commit_from<Cmd>(m.frozen.harvest());   // was Cmd::commit_scrollback (none if empty)
return Cmd(SetTitle{"build: ok"});             // was Cmd::set_title
```

## Checking a port

```sh
python3 tests/smoke.py build/maya_<name>               # one program
python3 tests/smoke.py build/maya_<name> --animates=5  # ...that animates after key 5
sh tests/smoke_all.sh build                                 # every example
python3 tests/screen_after.py build/maya_<name> "<down><down>"   # special keys, prints the screen
python3 tests/snapshot.py build/maya_<name> out.png         # a pixel demo, as a PNG
```

The harness runs the binary in a real pty and checks that it draws a first
frame, reacts to a key, survives a resize, doesn't spin while idle, quits
with 0 and restores the terminal. `--animates=KEY` presses KEY and then
sends nothing: the screen has to keep changing on its own.
