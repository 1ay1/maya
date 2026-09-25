# Moving a maya Program onto jaal

maya ships two loops for the same toolkit:

- `maya::run<P>()` is maya's own event loop.
- `maya::run_jaal<P>()` (in `<maya/jaal/host.hpp>`, built with
  `-DMAYA_WITH_JAAL=ON`) keeps maya's terminal, input parser and renderer and
  swaps only the loop underneath for jaal (the `third_party/jaal` submodule).

Your `view()`, widgets, DSL and themes don't change. The model, messages,
`update`, effects and subscriptions change shape, because jaal checks at
compile time what maya only checks at runtime, or never checks at all.
Every `examples/jaal_*.cpp` is a port of the example with the same name, so
you can read any original next to its port. This page covers the
transformations those 21 ports needed.

## 1. The program shape

| maya                                                  | jaal                                                    |
|-------------------------------------------------------|---------------------------------------------------------|
| `static Model init()`                                 | `Model{}` (default-constructed), plus an optional `static Cmd init(Model&)` |
| `static auto update(Model, Msg) -> pair<Model, Cmd>`  | one `static Cmd update(Model&, Case)` **per alternative** |
| `std::visit(overload{...}, msg)`                      | gone: jaal dispatches to the matching overload          |
| `Cmd<Msg>`, `Sub<Msg>`                                | `using Cmd = jaal::Cmd<Msg, extra fx...>;` `using Sub = jaal::Sub<Msg, sources...>;` |
| `static_assert(Program<P>)`                           | `static_assert(maya::JaalView<P>)`                      |
| `run<P>(cfg)`                                         | `return run_jaal<P>(cfg);` (same `RunConfig`, returns the exit code) |

Before, from `examples/counter.cpp`:

```cpp
static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
    return std::visit(overload{
        [&](Increment) { return std::pair{Model{m.count + 1}, Cmd<Msg>{}}; },
        [](Quit)       { return std::pair{Model{}, Cmd<Msg>::quit()}; },
    }, msg);
}
```

After:

```cpp
using Cmd = jaal::Cmd<Msg>;
static Cmd update(Model& m, Increment) { ++m.count; return {}; }
static Cmd update(Model&,   Quit)      { return Cmd::quit(0); }
```

If you leave out an overload, the program doesn't compile, because jaal's
`Program` concept requires every message to have a handler. maya's
`overload{}` catches the same mistake. The difference is that the jaal
version also checks nested message groups (see §7).

## 2. The rows: say what you use

A jaal `Cmd` and `Sub` carry a **row**, a list of the effect and source
kinds the program may use. The core effects (`quit`, `send`, `after`,
`task`, `now`, `random`) and the core sources (`every`, `stream`) are
always included. Anything the *host* provides has to be listed:

```cpp
using Cmd = jaal::Cmd<Msg, commit_scrollback, set_title>;   // terminal effects
using Sub = jaal::Sub<Msg, on_key, on_mouse, on_resize, jaal::fx::on_signal>;
```

The maya host provides `on_key`, `on_mouse`, `on_paste`, `on_focus` and
`on_resize` (sources) plus `commit_scrollback` and `set_title` (effects).
A host that can't provide one (a test host, a GUI) rejects the program at
compile time, where maya would fail at runtime.

## 3. Subscriptions

| maya                          | jaal                                                         |
|-------------------------------|--------------------------------------------------------------|
| `key_map<Msg>({{'q', Quit{}}})` | `jaal_key_map<Sub>({{'q', Quit{}}})` (same table, same `key_is` matching) |
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
argument of `run_jaal`) and then detaches
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
port (`examples/jaal_agent_session.cpp`) splits the work **at the gate**:

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

- `RunConfig{.fps = N}` works the same way: jaal renders continuously at N
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
return commit_from<Cmd>(m.frozen.harvest());   // maya: Cmd::commit_scrollback (none if empty)
return Cmd(SetTitle{"build: ok"});             // maya: Cmd::set_title
```

## Checking a port

Build with `-DMAYA_WITH_JAAL=ON`, then:

```sh
python3 tests/jaal_smoke.py build-jaal/jaal_<name>              # one program
python3 tests/jaal_smoke.py build-jaal/jaal_<name> --animates=5  # ...that animates after key 5
sh tests/jaal_smoke_all.sh build-jaal                            # all 21
```

The harness runs the binary in a real pty and checks that it draws a first
frame, reacts to a key, survives a resize, doesn't spin while idle, quits
with 0 and restores the terminal. `--animates=KEY` presses KEY and then
sends nothing: the screen has to keep changing on its own.
