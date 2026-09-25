# The new API, in one page (for doc writers)

maya has ONE way to run a program: a jaal program run by `maya::run<P>()`.
Include `<maya/app.hpp>` (which includes `<maya/maya.hpp>`), link `maya::app`.
Everything below is verified against the code.

```cpp
#include <maya/app.hpp>
using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

struct Counter {
    struct Model { int count = 0; };                   // default-constructible; ALL state
    struct Inc {}; struct Dec {}; struct Tick {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Tick, Quit>;    // one struct per message

    using Cmd = jaal::Cmd<Msg>;                        // + terminal effects used, e.g. jaal::Cmd<Msg, set_title>
    using Sub = jaal::Sub<Msg, on_key>;                // + every event source used

    static Cmd init(Model&) { return {}; }             // optional
    static Cmd update(Model& m, Inc)  { ++m.count; return {}; }   // one per alternative
    static Cmd update(Model& m, Dec)  { --m.count; return {}; }
    static Cmd update(Model& m, Tick) { return {}; }
    static Cmd update(Model&,   Quit) { return Cmd::quit(0); }

    static Element view(const Model& m) { ... }         // pure

    static Sub subscribe(const Model& m) {             // diffed every step
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            keys<Sub>({{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}}}));
    }
    static bool subs_key(const Model& m) { return true; }  // optional: what subscribe() depends on
};

int main() { return run<Counter>({.title = "counter"}); }
```

- `static_assert(Program<Counter>);` checks the shape (concept `maya::Program`).
- `run<P>(Options)`; `Options` fields: `title`, `fps` (0 = event driven; >0 redraw continuously), `mouse`, `hover_motion`, `mode` (`Mode::Fullscreen` default, `Mode::Inline` renders into scrollback), `backend`, `theme`, `enhanced_keyboard`.
- Event sources (list in Sub row): `on_key` (KeyEvent), `on_mouse` (MouseEvent{button, kind: Press/Release/Move, x.value, y.value 1-based, mods}), `on_paste` (PasteEvent{content}), `on_focus`, `on_resize` (ResizeEvent{width.value, height.value}). Use `Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> { ... })` — the lambda must be CAPTURELESS.
- `keys<Sub>({{'q', Quit{}}, {SpecialKey::Up, Up{}}})`: plain key, no modifiers. Predicates for richer handlers: `key_is(k,'c')`, `key_is(k, SpecialKey::Up)`, `ctrl_is(k,'c')`, `alt_is(k,'x')`.
- Core jaal effects: `Cmd::quit(code)`, `Cmd::after(duration, Msg)`, `Cmd::task(fn, args..., mapper)` (runs on a worker thread; see jaal docs), `Cmd::batch(...)`. Core sources: `Sub::every(period, Msg)`, `Sub::stream(...)`.
- Terminal effects (add to the Cmd row to use them): `set_title` (`SetTitle{"x"}`), `write_clipboard`, `query_clipboard`, `emit_host_sequence`, `commit_scrollback` (`commit_from<Cmd>(ledger.harvest())`), `commit_overflow`, `force_redraw`, `reset_inline`, `set_mouse` (`SetMouse{bool}`), `suspend` (hand the tty to a child like $EDITOR). `terminal_cmd<Msg>` = a Cmd with all of them.
- Scroll views: keep `mutable ScrollState` in the Model; the Screen forwards mouse wheel/drag to painted ScrollStates automatically; keys are the program's: route with a message and `state.handle(key, viewport_h)` in update.
- Pixel graphics: `#include <maya/element/pixels.hpp>`; `Image img(w, h)` (h = 2 x rows, half blocks), `img(x,y) = Rgb{r,g,b}`, `img.fill_rows([&](int x, int y) -> Rgb {...})` (parallel rows), `pixels(std::move(img))` is an element. `Glyphs` + `glyphs(grid)` for coloured character art. There is NO Canvas callback API any more (canvas_run is deleted).
- Static output without a runtime: `#include <maya/print.hpp>` (in maya.hpp): `print(element)`, `print(element, width)`, `render_to_string(element, width)`.
- DELETED (must not appear in docs except in a "what changed" note): `maya::run(cfg, event_fn, render_fn)`, `run(event_fn, render_fn)`, `canvas_run`, `CanvasConfig`, `live()`, `LiveConfig`, `maya::Cmd<Msg>`, `maya::Sub<Msg>`, `key_map<Msg>`, `maya::quit()`, `maya::set_mouse()`, `RunConfig` (now `Options`), `Ctx`, `init() -> pair<Model, Cmd>`, `update(Model, Msg) -> pair<Model, Cmd>`, `std::visit(overload{...})` dispatch, `MAYA_WITH_JAAL`, `maya/jaal/host.hpp`, `run_jaal`, `JaalView`, `jaal_key_map`, `jaal_` example prefix.
- Examples (all in examples/, 54): counter (quickstart), stopwatch, agent_stats, viewport, messenger, particles, mandelbrot, doom_fire, fluid, breakout, snake, raymarch, fps, space3d, spectrum, dashboard, sysmon, stocks, chat, music, scroll_2d/clip/slice/styles, inline_progress (Mode::Inline), terminal_fx (terminal effects), ...
- Design: docs/internals/design.md. maya is to jaal what Ink is to React.
