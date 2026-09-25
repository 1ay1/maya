#pragma once
// maya/jaal/canvas.hpp — canvas animations as jaal programs.
//
// A canvas demo (a fire, a fluid, a ray tracer) used to need its own loop,
// maya's canvas_run(): input, pacing, resize, a double buffer, a diff, a
// writer, all duplicated from the Program loop. In the runtime-free design
// it is an ordinary jaal program:
//
//   * a `paint` element (maya/element/builder.hpp) that fills the screen and
//     draws cells directly - the view,
//   * `Sub::every(1s / fps, Tick{})` - the pacing (jaal keeps phase and
//     resyncs after a stall),
//   * events routed to the demo's handler - the input.
//
// run_canvas() keeps canvas_run's three-callback shape, so a demo moves by
// changing one call:
//
//     canvas_run(CanvasConfig{.fps = 60, .title = "fire"}, rebuild, handle, paint);
//  -> return run_canvas({.fps = 60, .title = "fire"}, rebuild, handle, paint);
//
// The one contract the adapter owns: style ids. A demo interns its styles in
// on_resize(pool, w, h) and paints with the cached ids. Those ids belong to
// the Screen's StylePool, which can be REBASED (cleared) when a truecolor
// animation nears the 16-bit id space. So on_resize runs again whenever the
// pool it interned against is gone (its pool_id changed), not only when
// the size changes. A canvas_run demo could never see that happen, because
// canvas_run owned a private pool; here it's explicit.

#if !MAYA_WITH_JAAL
#error "maya/jaal/canvas.hpp needs MAYA_WITH_JAAL (link maya::jaal)"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <variant>

#include "host.hpp"

namespace maya {

namespace detail::canvas_prog {

// The demo's callbacks, type-erased once. They are ordinary functions of the
// demo's own (global) state, called only on the loop thread: exactly how
// canvas_run called them.
struct Callbacks {
    std::function<void(StylePool&, int, int)>     on_resize;
    std::function<bool(const Event&)>             on_event;
    std::function<void(Canvas&, int, int)>        on_paint;
    int  fps  = 60;
    bool clear = true;
};
inline Callbacks& callbacks() { static Callbacks c; return c; }

// The program's side of MAYA_INPUT_LOG (maya logs the bytes and parsed
// events; this logs what the demo's own handler received and decided), so
// one file shows a key's whole path: wire -> parser -> program -> quit.
inline void trace(const char* what) {
    static std::FILE* const f = [] () -> std::FILE* {
        const char* p = std::getenv("MAYA_INPUT_LOG");
        return (p && *p) ? std::fopen(p, "a") : nullptr;
    }();
    if (!f) return;
    std::fprintf(f, "            %s\n", what);
    std::fflush(f);
}

}  // namespace detail::canvas_prog

// The program. Its model is the frame counter: a Tick changes it, so the
// host draws; the demo's own state lives where it always did.
struct CanvasProgram {
    struct Model { std::uint64_t frame = 0; };
    struct Tick {};
    struct Input { Event ev; };
    struct Quit {};
    using Msg = std::variant<Tick, Input, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_mouse, on_paste, on_focus>;

    static Cmd update(Model& m, Tick)    { ++m.frame; return {}; }
    static Cmd update(Model& m, Input i) {
        // canvas_run's contract: the handler returns false to quit. Every
        // event may change what the next frame shows, so it counts as a
        // model change and is drawn on the next tick (or right now, for a
        // demo paced slower than input).
        const bool keep = detail::canvas_prog::callbacks().on_event(i.ev);
        detail::canvas_prog::trace(keep ? "handler: keep running" : "handler: QUIT -> Cmd::quit(0)");
        if (!keep) return Cmd::quit(0);
        ++m.frame;
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    static Element view(const Model&) {
        return detail::paint([](Canvas& c, int x, int y, int w, int h) {
            auto& cb = detail::canvas_prog::callbacks();
            // Re-intern whenever the size OR the pool changed (see the
            // header: the Screen's pool may be rebased).
            static int last_w = -1, last_h = -1;
            static std::uint64_t last_pool = 0;
            StylePool& pool = *c.style_pool();
            if (w != last_w || h != last_h || pool.pool_id() != last_pool) {
                cb.on_resize(pool, w, h);
                last_w = w; last_h = h; last_pool = pool.pool_id();
            }
            // The demo paints in its own (0, 0)-based coordinates over the
            // whole screen; the paint element is the whole screen, so x/y
            // are 0 and the clip is the screen.
            (void)x; (void)y;
            if (cb.clear) for (int yy = 0; yy < h; ++yy)
                              for (int xx = 0; xx < w; ++xx) c.set(xx, yy, U' ', 0);
            cb.on_paint(c, w, h);
        });
    }

    static Sub subscribe(const Model&) {
        // Pacing is the HOST's job (run_canvas passes fps as the host's frame
        // rate: a nanosecond clock that keeps phase). A Sub::every here would
        // take whole milliseconds: 1000/60 truncates to 16 ms, and each
        // tick then waits for the one before it to be drawn, which measured
        // 54.7 fps where the demo asks for 60.
        auto input = [](const auto& e) -> std::optional<Msg> { return Input{Event{e}}; };
        return Sub::batch(
            Sub::on(on_key{},   [=](const KeyEvent& e)   { return input(e); }),
            Sub::on(on_mouse{}, [=](const MouseEvent& e) { return input(e); }),
            Sub::on(on_paste{}, [=](const PasteEvent& e) { return input(e); }),
            Sub::on(on_focus{}, [=](const FocusEvent& e) { return input(e); }));
    }
    static bool subs_key(const Model&) { return true; }   // subscriptions never change
};

/// canvas_run, on jaal. Same config and callbacks; returns an exit code.
template <CanvasResizeFn ResizeFn, CanvasEventFn EventFn, CanvasPaintFn PaintFn>
int run_canvas(CanvasConfig cfg, ResizeFn&& on_resize, EventFn&& on_event, PaintFn&& on_paint) {
    auto& cb     = detail::canvas_prog::callbacks();
    cb.on_resize = std::forward<ResizeFn>(on_resize);
    cb.on_event  = std::forward<EventFn>(on_event);
    cb.on_paint  = std::forward<PaintFn>(on_paint);
    cb.fps       = cfg.fps;
    cb.clear     = cfg.auto_clear;
    return run_jaal<CanvasProgram>({.title = cfg.title, .fps = cfg.fps, .mouse = cfg.mouse,
                                     .mode = cfg.mode});
}

}  // namespace maya
