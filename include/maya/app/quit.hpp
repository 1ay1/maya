#pragma once
// maya::quit() — global quit flag for non-Program code
//
// Used by the simple run(event_fn, render_fn) convenience and live().
// Program apps should use Cmd<Msg>::quit() instead.

namespace maya {

namespace detail {
    inline thread_local bool quit_requested = false;
} // namespace detail

/// Request a clean exit from the simple run() or live() loop.
/// For Program apps, use Cmd<Msg>::quit() instead.
inline void quit() noexcept { detail::quit_requested = true; }

} // namespace maya
