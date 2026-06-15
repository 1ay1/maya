#pragma once
// maya::quit() — global quit flag for non-Program code
//
// Used by the simple run(event_fn, render_fn) convenience and live().
// Program apps should use Cmd<Msg>::quit() instead.

namespace maya {

namespace detail {
    inline thread_local bool quit_requested = false;
    // Pending runtime mouse-capture toggle, applied by the run()/Program
    // loops each iteration: -1 = none, 0 = disable, 1 = enable.
    inline thread_local int  mouse_request  = -1;
} // namespace detail

/// Request a clean exit from the simple run() or live() loop.
/// For Program apps, use Cmd<Msg>::quit() instead.
inline void quit() noexcept { detail::quit_requested = true; }

/// Toggle terminal mouse reporting at runtime, from inside a run()/Program
/// handler. Disabling hands the scroll wheel back to the terminal (native
/// scrollback works again) at the cost of in-app clicks; enabling re-captures
/// them. Mouse capture and native terminal scroll are mutually exclusive —
/// this is the switch between them. Takes effect on the next loop iteration.
inline void set_mouse(bool on) noexcept { detail::mouse_request = on ? 1 : 0; }

} // namespace maya
