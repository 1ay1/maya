#pragma once
// maya/device/frame_request.hpp — a widget asks for the next frame while it animates;
// the host (<maya/host/run.hpp>) schedules it. A per-frame request, not a loop.

#include <chrono>
#include <cstdint>

#include "../core/motion.hpp"

namespace maya {

// ============================================================================
// request_animation_frame — widget-side "I'm animating, please redraw soon"
// ============================================================================
// Widgets that want a smooth animation call request_animation_frame()
// from their build() each frame the animation should continue. This is
// the ONE animation engine: it does not maintain its own clock or poll
// schedule. It records, in a per-render registry, that *something on
// screen wants to step at ~frame cadence*. The run loop collects these
// requests right alongside Sub::Every timers and folds them into the
// single "when should I next wake and render" computation — no separate
// deadline global, no parallel pump, no second poll clamp.
//
// The distinction from Sub::Every is intent, not mechanism: Every
// delivers a Msg (drives update → model), a frame request only asks for
// a repaint (pure visual layer — cursor blink, scramble caret, sigil
// fade — that reads wall-clock in build() and mutates nothing). Both
// resolve to the same wake schedule.
//
// Single-threaded UI; the registry is a thread-local cleared at the
// start of each render (mirrors live_scroll_states). A widget that
// stops calling drops out of the next collection → loop returns to
// idle wait → zero bytes per idle frame. Idempotent within a frame.

namespace detail {
// ~60 fps cap — the cadence at which a frame request asks the loop to
// re-render. Single source of truth for animation frame timing.
inline constexpr auto kAnimationFrameInterval = std::chrono::milliseconds{16};

// Per-render frame-request flag. A widget's build() sets this; the run
// loop reads it after view()/render() to decide whether to schedule a
// follow-up frame, then clears it for the next render. Thread-local,
// single-threaded UI — same ownership model as live_scroll_states.
inline thread_local bool animation_requested_ = false;

// Companion to animation_requested_: the frame-delay policy for THIS render.
// Sentinel -1 = "unset" (no request yet this frame). 0 = at least one FAST
// requester (plain request_animation_frame / the motion framework's
// request_frame) wants the default ~16 ms / 60 fps cadence. A positive value
// = every requester so far opted into a minimum delay, and this is the
// SMALLEST such delay (the finest slow cadence any live widget needs).
//
// Merge rule (see the two setters below): a FAST request pins this to 0 and
// nothing can raise it again this frame — 60 fps for the welcome bob /
// spinner / streaming reveal must always win over a slow widget (the 265 ms
// caret blink) that happens to render in the same frame. Only when EVERY
// requester this frame asked for a delay does the loop sleep longer. Reset
// to -1 each render alongside animation_requested_.
inline thread_local std::int64_t next_frame_delay_ms_ = -1;

}  // namespace detail

inline void request_animation_frame() noexcept {
    detail::animation_requested_ = true;
    // A plain request wants the fast default cadence. Pin the policy to 0
    // (fast) so no slow request_animation_frame_after() this frame can raise
    // it — the welcome bob / spinner / streaming reveal always win over a
    // co-live slow widget (the caret blink).
    detail::next_frame_delay_ms_ = 0;
}

// Request the next frame no sooner than `delay_ms` from now (instead of the
// default ~16 ms). For SLOW, self-driving animations (the composer caret
// blink at its 265 ms half-period) so the run loop sleeps between visible
// steps rather than waking at 60 fps to re-check an effect that toggles a
// few times a second. Implies request_animation_frame().
//
// A delay only takes effect if EVERY requester this frame opted in: a single
// fast request (delay 0, pinned by request_animation_frame) dominates and is
// never raised. Among competing delays the SMALLEST wins (finest slow
// cadence any live widget needs).
inline void request_animation_frame_after(std::int64_t delay_ms) noexcept {
    detail::animation_requested_ = true;
    if (delay_ms < 0) delay_ms = 0;
    auto& cur = detail::next_frame_delay_ms_;
    if (cur == 0) return;                 // a fast requester already won
    if (cur < 0 || delay_ms < cur) cur = delay_ms;   // unset, or a finer delay
}

// Is a widget already driving the next frame itself?
//
// True when something called request_animation_frame(_after) during the last
// build(). Those frames are guaranteed to render — the run loop's RAF
// override bypasses the visual-hash gate for exactly this case — and the
// loop is already scheduled to wake on the requesting widget's own cadence.
//
// This exists so a host's visual_hash can DEFER instead of guessing. The
// hash's job is to describe host-paced state: model fields, and time buckets
// for animations the host itself drives with a timer. For a widget-paced
// (RAF) visual it must add NO time term — a host bucket is a second,
// independent clock for one visual, and unless it exactly matches the
// widget's interval the two beat, so renders land mid-step and smooth motion
// turns into stutter.
//
// The rule a host can now express directly rather than remember:
//
//     if (!maya::animation_pending()) {
//         // only bucket time for animations WE pace
//         mix(now_ms / our_timer_period);
//     }
//
// agentty hit the failure this prevents: it bucketed the RAF-driven welcome
// screen at its own 80 ms tick while the widget asked for 110 ms, producing
// 42 hash values for 30 requested frames and a visibly flickering idle
// screen.
[[nodiscard]] inline bool animation_pending() noexcept {
    return detail::animation_requested_;
}

// Wire the decoupled motion-framework frame-request hook to the host. anim::Motion / Timeline / pulse (core/motion.hpp) wake the loop
// WITHOUT depending on this 90 KB header by routing through
// anim::detail::raf_hook, installed here at static-init so any TU that links
// the app gets self-driving animations for free.
namespace detail {
inline void raf_thunk_() noexcept { ::maya::request_animation_frame(); }
inline void raf_after_thunk_(std::int64_t delay_ms) noexcept {
    ::maya::request_animation_frame_after(delay_ms);
}
inline const ::maya::anim::detail::RafInstaller raf_installer_{
    &raf_thunk_, &raf_after_thunk_};
} // namespace detail

} // namespace maya
