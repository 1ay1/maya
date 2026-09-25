# Porting a maya example to the new API

maya has ONE way to run a program now: a jaal program, run by
`maya::run<P>(Options)` from `<maya/app.hpp>`. The old loops (`run(cfg,
event_fn, render_fn)`, `canvas_run`, `live`, `maya::Cmd`, `maya::Sub`,
`key_map<Msg>`, `maya::quit()`) are DELETED. Read docs/internals/design.md.

## Program shape (the only shape)

```cpp
#include <maya/app.hpp>
#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

struct Model { ... };                        // default-constructible; ALL state
struct Tick {}; struct Quit {}; ...          // one struct per message
using Msg = std::variant<Tick, Quit, ...>;

struct Demo {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;                       // + terminal effects if used, e.g. jaal::Cmd<Msg, set_mouse>
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;    // list every event source used: on_key, on_mouse, on_paste, on_focus, on_resize

    static Cmd init(Model&) { ... }                      // optional
    static Cmd update(Model& m, Tick) { ...; return {}; } // ONE overload per alternative
    static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
    static Element view(const Model& m) { ... }          // pure: reads m only
    static Sub subscribe(const Model& m) {
        return Sub::batch(
            Sub::every(33ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> { return Resize{r.width.value, r.height.value}; }),
            keys<Sub>({{'q', Quit{}}, {SpecialKey::Escape, Quit{}}, {SpecialKey::Up, Up{}}}));
    }
    static bool subs_key(const Model& m) { return m.animating; }  // what subscribe() depends on (true/const if nothing)
};

static_assert(Program<Demo>);

}  // namespace

int main() { return run<Demo>({.title = "demo"}); }   // Options: title, fps, mouse, hover_motion, mode (Mode::Fullscreen/Inline), theme
```

Rules (these are the point of the rewrite; follow them):
1. NO globals / file-scope mutable statics. Everything lives in Model. Constant tables (`constexpr`, `const` lookup tables) are fine.
2. `view()` is pure: it only reads the model. No RNG, no time, no mutation in view. Simulation happens in `update(Model&, Tick)`.
3. Animation = `Sub::every(period, Tick{})` in subscribe(). If the demo can be still (paused, nothing moving), don't subscribe the timer then, and make `subs_key` reflect it.
4. The lambdas passed to `Sub::on` / `Sub::every` must be CAPTURELESS (they run on the loop; state comes from the model via messages).
   For a key handler richer than a table, use `Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> { if (key_is(k,'q')) return Quit{}; ... return std::nullopt; })`. Predicates: `key_is(k, 'c')`, `key_is(k, SpecialKey::Up)`, `ctrl_is(k, 'c')`, `alt_is(k,'x')`; `std::get_if<CharKey>(&k.key)->codepoint` for any char.
   Mouse: `Sub::on(on_mouse{}, [](const MouseEvent& e) -> std::optional<Msg> {...})`; MouseEvent{button (MouseButton::Left/ScrollUp/...), kind (MouseEventKind::Press/Release/Move), x.value, y.value (1-based), mods}. Pass `.mouse = true` in Options.
5. Randomness: keep a `std::mt19937 rng{seed}` IN the model and use it in update.
6. Terminal size: handle `on_resize` into the model (ResizeEvent.width.value / .height.value, in cells) — the first resize arrives at startup.
7. Scroll views (`ScrollState` + `| scroll(state,...)`): the Screen auto-dispatches wheel/drag to live ScrollStates, so keep the ScrollState IN the model (by value). Arrow-key scrolling becomes messages that mutate it in update.
8. Pixel/canvas demos: build an `Image` (`#include <maya/element/pixels.hpp>`: `Image img(w, h); img(x, y) = Rgb{r,g,b};` h = 2*rows because half blocks; `img.fill_rows([&](int x,int y)->Rgb{...})` for per-pixel pure functions, runs rows in parallel) and return `pixels(std::move(img))` from view. For character art use `glyphs()` from the same header (read its doc comment). Compose with a status bar: `v(pixels(img), status_bar(m))`. Keep colours to <= ~5 bits/channel where cheap (& 0xF8) so the renderer's style cache stays hot.
   If a canvas demo drew text/box glyphs at arbitrary cells, prefer rebuilding it from elements (h/v/text/border) or glyphs(); do not reintroduce Canvas callbacks.
9. Keep the demo's features, keys and look. Keep the header comment (update it: describe Model / update / view briefly like examples/particles.cpp does). Keep it roughly the same length or shorter.
10. Style reference: examples/particles.cpp, examples/mandelbrot.cpp, examples/fluid.cpp, examples/doom_fire.cpp (canvas-style), examples/counter.cpp, examples/stopwatch.cpp, examples/agent_stats.cpp (element-style).

## Build + verify (MANDATORY)

    cd /Users/ayush/projects/maya && ninja -C build-app maya_NAME 2>&1 | grep -E "error|warning: unused" | head -30

Iterate until it compiles with no errors. Then smoke test it:

    python3 tests/jaal_smoke.py build-app/maya_NAME --keys "<2-3 keys that visibly change the screen>"

All checks must say ok. Do not run ninja on other targets or the whole tree (other agents are building in parallel; build only your own targets). Do not edit anything outside the files you were given. Do not git commit.
