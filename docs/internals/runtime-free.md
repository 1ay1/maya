# maya without a runtime

maya is a terminal UI toolkit. This document is the plan for making it
*only* that: a view layer and a terminal device, with no event loop, no
timers, no worker threads and no effect types. The runtime is
[jaal](../../third_party/jaal/README.md), and maya plugs into it through
a thin host adapter that lives in its own target.

## The problem, as found

The case for this rests on bugs, not taste. Porting 21 examples to jaal
and profiling all 49 turned up the following, and every one of them
lives in code that belongs to a *runtime*, not to a *renderer*:

| Found | Where | Kind |
|---|---|---|
| SIGSEGV on quit in `messenger`: the quit step rendered the reset (empty) model | `run<P>` | loop |
| Hang on quit in `agent_session`: `~BackgroundQueue` joined a worker that couldn't be asked to stop | task queue | threads |
| `RunConfig::theme` dropped at startup on every loop | `publish_theme_slot` | loop setup |
| jaal host missing the theme canvas, scroll write-back and warmup | host vs `run<P>` | three loops drifted |
| A frame drawn for every event the program ignored | jaal `run()` | loop |
| `canvas_run` had no profiling hook; fullscreen `run<P>` had none either | two loops | drift |

maya has **three** loops (`run<P>`, the callback `run(event_fn,
render_fn)`, `canvas_run`) plus a task queue, plus `maya::Cmd`/`Sub`,
which duplicate jaal's typed effects without jaal's guarantees
(`Cmd::task` takes a capturing `std::function`, the exact thing jaal's
`Sendable` exists to reject). Each loop re-implements frame pacing,
resize, animation scheduling and write-backpressure, and they drift.

Meanwhile the parts that *made things fast* in this work all live in the
view layer: the relayout fast path (widgets 1.52 → 0.80 ms/frame), the
look-aware diff, CUF instead of CUP, the memoised colour quantizer
(doom_fire over ssh 99% → 17% CPU). Both runtimes got them for free.
That is the split the code is already telling us to make.

## The layers

```
┌────────────────────────────────────────────────────────────────────┐
│ app          your Program: Model, Msg, update, view, subscribe     │
├────────────────────────────────────────────────────────────────────┤
│ jaal         the runtime: loop, timers, tasks, streams, signals,   │
│              shutdown, replay. Typed effects (Cmd/Sub rows).       │
├────────────────────────────────────────────────────────────────────┤
│ maya-jaal    the host adapter (~250 lines). Depends on both.       │
│              Turns jaal's calls into Terminal calls and back.      │
├────────────────────────────────────────────────────────────────────┤
│ maya         view:     Element, DSL, layout, paint, widgets,       │
│                        theme, colour. Pure: model -> cells.        │
│              device:   Terminal. Raw mode, inline/fullscreen,      │
│                        input parsing, frame encoding, scrollback.  │
│                        Stateful, non-blocking, never loops.        │
└────────────────────────────────────────────────────────────────────┘
```

**The rule:** maya never waits. It has no `poll`, no `sleep`, no thread,
no timer, no quit flag. Anything that needs time passing or a thread is
the runtime's business, and maya reports what it needs instead of doing
it.

## The device

`maya::Terminal` is today's `detail::Runtime` made public, minus its
loop-shaped parts. Every method is a single non-blocking step:

```cpp
class Terminal {
public:
    static Result<Terminal> open(const TermConfig&);   // raw mode, alt screen or inline, probes

    // input: the runtime watches the handle and calls read() when ready
    NativeHandle input_handle() const noexcept;
    Result<std::vector<Event>> read();                 // what's there now; never blocks
    void on_resize();                                  // after SIGWINCH
    Size size() const noexcept;

    // output: one frame
    Frame present(const Element& root);                // layout + paint + diff + write-or-buffer
    bool flush();                                      // push what the tty refused; false = still full

    // device effects, one call each
    void set_title(std::string_view);
    void write_clipboard(std::string_view);  void query_clipboard();
    void emit_host_sequence(std::string_view);
    void commit_scrollback(ScrollbackDebt);  void commit_overflow();
    void reset_inline();  void force_redraw();
    template <std::invocable F> auto suspend(F&& run_child);   // hand the tty to a child

    ~Terminal();                                       // restores the terminal, always
};
```

### `Frame`: what a draw needs from the runtime

The view layer used to talk to the loop through thread-local globals
that each loop had to remember to read (`animation_requested_`,
`next_frame_delay_ms_`, `scroll_writeback_dirty`). The jaal host forgot
two of them. They become the return value of the one call that
produces them:

```cpp
struct Frame {
    std::optional<Clock::time_point> redraw_at;   // a widget asked to be drawn again (animation)
    bool redraw_now    = false;                   // scroll write-back: this frame used stale sizes
    bool backpressured = false;                   // bytes left over: call flush() when writable
    std::size_t bytes  = 0;                       // what went to the wire (profiling)
};
```

A runtime that ignores `Frame` compiles and draws, but a scheduler
cannot *forget* a field it has to destructure to use. The widgets still
say "draw me again" the same way (`request_animation_frame()`), and the
collection point moves from "whichever loop reads the global" to
`present()`'s return value.

### What moves out of maya

| Today | Becomes |
|---|---|
| `run<P>()`, `run(event_fn, render_fn)`, `canvas_run()` | jaal programs; `maya-jaal` provides `run<P>` over jaal |
| `detail::BackgroundQueue`, `Cmd::task` | `jaal::Cmd::task` (captureless, `Sendable` args, stop token) |
| `maya::Cmd` / `maya::Sub` | `jaal::Cmd` / `jaal::Sub`; terminal effects are `maya-jaal` descriptors |
| `quit()`, `set_mouse()` globals | messages / effects (`Cmd::quit`, a `set_mouse` effect) |
| frame pacing, `fps`, resize debounce | jaal timers and the host's frame deadline |

### What stays in maya

Everything that turns a model into bytes, and nothing else: `Element`,
DSL, layout, paint, widgets, themes, colour, the diff/serialize encoder,
input parsing, and `print(element)` for one-shot output (it's a device
write, not a loop).

## Canvas animations and callback apps

The 14 `canvas_run` demos and 13 callback-`run` apps don't need a loop
of their own. On jaal they are ordinary programs:

- a **canvas animation** is `Sub::every(1s / fps, Tick{})` plus a view
  that is a `canvas_element` painting into the frame; `update(Tick)`
  advances the simulation.
- a **callback app** is a Program with one message (`Event`) whose
  update is the old event function.

`maya-jaal` provides both as tiny adapters so the demos keep their
shape, and the jaal host already does the rest (fps pacing, animation
frames, backpressure).

## Order of work

1. `Frame` replaces the loop-facing globals; `Terminal` is the public
   device. Existing loops keep working on top of it (no behaviour
   change, measured).
2. `maya-jaal` becomes its own target over `Terminal`.
3. Port the remaining 27 examples (canvas and callback adapters).
4. Delete the three loops, the task queue and `maya::Cmd`/`Sub`.

Each step keeps the suite green and the jaal smoke run at N/N.
