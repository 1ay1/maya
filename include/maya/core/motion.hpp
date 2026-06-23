#pragma once
// maya::anim::motion — The declarative animation FRAMEWORK
// ============================================================================
//
// `animation.hpp` gives you the MATH: Tween, Spring, Animated, RateCursor,
// easing curves, lerp. Those are pure value types — you tick them by hand,
// you read the clock yourself, you decide when to ask for another frame.
//
// `motion.hpp` gives you the FRAMEWORK around that math so a widget author
// never writes frame plumbing again. The design goal: animating ANYTHING —
// a colour fade, a slide-in, a staggered list, a multi-step intro
// choreography — should be a few declarative lines that read like intent,
// with the library owning time, ticking, frame-request cadence, and
// lifecycle.
//
// Before motion.hpp every animated widget repeated the same three chores:
//
//     auto now = steady_clock::now();              // 1. read the clock
//     double dt = (now - last_) / 1000.0; last_=now;//   compute + clamp dt
//     value_.tick(dt);                             // 2. tick the math
//     if (!value_.done()) request_animation_frame();// 3. keep the loop awake
//
// — copy-pasted into composer, welcome_screen, markdown, spinner, … each
// with its own subtly-different remount + dropped-frame + throttle logic.
// motion.hpp folds all of that into ONE place.
//
// ── The layers ──────────────────────────────────────────────────────────
//
//   Clock         A frame-time source. now_ms() + a self-computing dt()
//                 with dropped-frame clamping and remount detection. One
//                 process-wide default clock backs everything; you rarely
//                 touch it directly.
//
//   Motion<T>     The headline type. A self-driving animated value:
//                     Motion<Color> tint{Color::white()};
//                     tint.to(Color::red(), 0.3s);   // retarget, momentum-safe
//                     el | fg(tint.get());           // read in build()
//                 .get() ticks against the shared clock, auto-requests the
//                 next frame while in flight, and is a no-op once settled.
//                 No dt, no clock, no RAF in the widget. Ever.
//
//   Timeline      Choreography: keyframes, delays, parallel + sequential
//                 composition, loops. For "no matter how complicated" —
//                 multi-step intros, cascade reveals, ping-pong loops.
//
//   Stagger       Index-phased fan-out: the same Motion shape applied to N
//                 children each offset by a per-item delay (list mount,
//                 menu open, typewriter-of-rows).
//
// Everything is header-only, allocation-free on the steady-state path, and
// constexpr where the math allows. Reading a settled Motion is a single
// branch; an in-flight Motion costs one tick + one bool. Safe to hold
// hundreds of them.
//
// ── frame-request hook ───────────────────────────────────────────────────
// Motion needs to tell the run loop "keep painting, I'm moving." That is
// maya::request_animation_frame(), declared in app.hpp. To keep motion.hpp
// from pulling in the 90 KB app header, we route through a function pointer
// (anim::detail::raf_hook) that app.hpp installs at static-init time. If no
// host is present (unit test, headless use) the hook is null and Motion
// still ticks correctly off the clock — it just doesn't nudge a loop that
// isn't there.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "animation.hpp"

namespace maya::anim {

// ============================================================================
// frame-request hook — decoupled from app.hpp
// ============================================================================
namespace detail {

// Installed by app.hpp (see request_animation_frame there). When a Motion
// is in flight it calls this so the run loop schedules another paint. Null
// in headless/unit-test contexts — Motion still advances off the Clock, it
// just has no loop to wake.
using RafHook = void (*)() noexcept;
inline RafHook raf_hook = nullptr;

inline void request_frame() noexcept {
    if (raf_hook) raf_hook();
}

// RAII installer so app.hpp can wire the hook with a one-liner at namespace
// scope: `inline anim::detail::RafInstaller _maya_raf{&request_animation_frame};`
struct RafInstaller {
    explicit RafInstaller(RafHook h) noexcept { raf_hook = h; }
};

} // namespace detail

// ============================================================================
// Clock — the one frame-time source
// ============================================================================
// Wraps steady_clock and answers two questions a widget keeps asking:
//
//   now_ms()  — monotonic wall time in ms since the clock's epoch. Use it
//               for phase-driven effects (sin(now), blink buckets) that
//               don't need a delta.
//
//   dt()      — seconds since the LAST dt() call on this clock instance.
//               This is what Motion feeds to the math each frame. It:
//                 • clamps a long stall to kMaxStep so a backgrounded app
//                   doesn't teleport every animation on the first frame back;
//                 • detects a "remount" (a gap larger than kRemountGapMs,
//                   e.g. a screen that was off-view) and returns 0 for that
//                   frame so the resumed animation eases from rest, not from
//                   a multi-second leap.
//
// A single process-wide clock (default_clock()) backs every Motion that
// doesn't name its own — so all animations on a frame see the SAME dt and
// stay in lockstep (critical for staggers and parallel timelines to read as
// one coordinated motion rather than N independently-jittering ones). The
// shared clock ticks ONCE per frame: the first Motion to call dt() in a
// frame computes the real delta; subsequent reads in the same frame get the
// cached value (keyed on now_ms) so 200 Motions don't each see a sliver of
// dt. Call begin_frame() at the top of view() if you want to force the
// boundary explicitly; otherwise the now_ms-keyed cache handles it.
class Clock {
public:
    using clock_t = std::chrono::steady_clock;

    // Tunables.
    static constexpr double kMaxStep      = 0.25;  // s — dropped-frame clamp
    static constexpr double kRemountGapMs = 500.0; // gap ⇒ treat as remount

    Clock() noexcept : epoch_(clock_t::now()), last_(epoch_) {}

    // Monotonic ms since this clock's epoch. Cheap; no state mutation.
    [[nodiscard]] std::int64_t now_ms() const noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   clock_t::now() - epoch_)
            .count();
    }

    // Seconds since the previous frame, clamped + remount-aware. Cached per
    // frame (keyed on the integer ms) so every Motion reading it within one
    // frame gets the same delta.
    [[nodiscard]] double dt() noexcept {
        const auto now    = clock_t::now();
        const std::int64_t now_ms_v =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - epoch_)
                .count();
        if (now_ms_v == cached_dt_at_ms_) return cached_dt_;  // same frame

        const double raw_ms =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                now - last_)
                .count();
        last_ = now;

        double dt_s;
        if (raw_ms > kRemountGapMs) {
            dt_s = 0.0;                       // remount — ease from rest
        } else {
            dt_s = raw_ms / 1000.0;
            if (dt_s > kMaxStep) dt_s = kMaxStep;
            if (dt_s < 0.0)      dt_s = 0.0;
        }
        cached_dt_       = dt_s;
        cached_dt_at_ms_ = now_ms_v;
        return dt_s;
    }

    // Force a fresh frame boundary (optional — the now_ms cache usually
    // makes this unnecessary). Call at the top of view() if you prefer an
    // explicit barrier.
    void begin_frame() noexcept { cached_dt_at_ms_ = -1; }

private:
    clock_t::time_point epoch_;
    clock_t::time_point last_;
    double              cached_dt_       = 0.0;
    std::int64_t        cached_dt_at_ms_ = -1;
};

// The process-wide clock every unnamed Motion ticks against. Thread-local so
// the single-threaded UI owns its own; a worker thread that animates an
// off-screen scratch gets its own consistent timeline too.
[[nodiscard]] inline Clock& default_clock() noexcept {
    static thread_local Clock c;
    return c;
}

// ============================================================================
// Mount — "ms since this widget appeared" (one-shot intro timelines)
// ============================================================================
// Many widgets animate relative to when they MOUNTED — a splash that fades
// in, a sigil that draws on first appearance, a list that cascades. They
// don't have a retarget; they replay a fixed schedule from t=0 each time
// they come on screen. The pattern they all hand-rolled (welcome_screen,
// activity_indicator, …):
//
//     static time_point first, last; static bool init;
//     auto now = steady_clock::now();
//     if (!init || now - last > gap) first = now;     // remount detect
//     last = now; return ms(now - first);
//
// Mount captures exactly that. elapsed_ms() returns ms since the widget
// first built (or since it last came back after being off-screen for longer
// than the remount gap), and requests a frame while still animating up to an
// optional total duration. Hold one as a (mutable) member; call elapsed_ms()
// in build().
//
//   struct Splash {
//       mutable anim::Mount mount;
//       Element build() const {
//           int age = mount.elapsed_ms(1200);   // animate for 1.2s
//           double p = anim::ease::out_cubic(std::min(1.0, age/1200.0));
//           ...
//       }
//   };
class Mount {
public:
    using clock_t = std::chrono::steady_clock;
    static constexpr double kRemountGapMs = 500.0;

    // Milliseconds since mount. If `animate_for_ms > 0`, requests a frame
    // while age < animate_for_ms (so a one-shot intro plays then idles); if
    // 0, requests a frame every call (perpetual — caller decides when to
    // stop reading). A gap longer than kRemountGapMs since the previous call
    // is treated as a remount and restarts the clock from 0.
    [[nodiscard]] std::int64_t elapsed_ms(double animate_for_ms = 0.0) const {
        const auto now = clock_t::now();
        if (!started_ ||
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                now - last_)
                    .count() > kRemountGapMs) {
            first_   = now;
            started_ = true;
        }
        last_ = now;
        const std::int64_t age =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - first_)
                .count();
        if (animate_for_ms <= 0.0 ||
            static_cast<double>(age) < animate_for_ms) {
            detail::request_frame();
        }
        return age;
    }

    // Force the next elapsed_ms() to restart from 0.
    void remount() noexcept { started_ = false; }
    [[nodiscard]] bool mounted() const noexcept { return started_; }

private:
    mutable clock_t::time_point first_{};
    mutable clock_t::time_point last_{};
    mutable bool                started_ = false;
};

// ============================================================================
// Motion<T> — a self-driving animated value
// ============================================================================
// The type you reach for 95% of the time. Hold one as a (mutable) widget
// member; set its target with to()/snap(); read the live value with get().
// get() owns the entire frame chore: pull dt from the clock, tick the math,
// and — while still moving — nudge the run loop for another frame. Once
// settled get() is a pure read with no frame request, so the loop returns
// to idle and you pay nothing.
//
//   struct Toggle {
//       anim::Motion<double> x{0.0};           // 0 = off, 1 = on
//       void set(bool on) { x.to(on ? 1.0 : 0.0, 0.18); }
//       Element build() const {
//           int fill = int(x.get() * width);   // ← ticks + requests frames
//           ...
//       }
//   };
//
// Construction picks the integrator:
//   Motion<T>{v}                     tween mode, starts settled at v
//   Motion<T>::spring(v, preset)     spring mode (momentum-preserving)
//
// to(target, dur, curve)   retarget over a duration (tween) — continuous,
//                          no jump (continues from the current value).
// spring_to(target)        retarget a spring, preserving velocity.
// snap(v)                  jump instantly, kill motion.
//
// T is anything anim::lerp supports: float/double, and maya Color.
template <typename T>
class Motion {
public:
    constexpr Motion() = default;

    // Start settled at `initial` in tween mode (the common default).
    explicit constexpr Motion(T initial,
                              double default_dur = 0.25,
                              ease::Fn curve = ease::out_cubic) noexcept
        : value_(Animated<T>::tween(initial, initial, 0.0, curve)),
          default_dur_(default_dur < 0.0 ? 0.0 : default_dur),
          curve_(curve ? curve : ease::linear) {}

    // Spring-mode factory. Retargets via spring_to() preserve momentum.
    [[nodiscard]] static constexpr Motion spring(
        T initial, SpringParams params = spring_presets::snappy) noexcept {
        Motion m;
        m.value_  = Animated<T>::spring(initial, params);
        m.spring_ = true;
        return m;
    }

    // ── targeting ──────────────────────────────────────────────────────
    // Retarget over a duration (tween mode). Continues from the live value
    // so there is never a visual jump even mid-flight. dur<0 uses the
    // Motion's default duration; curve=nullptr keeps the current curve.
    void to(T target, double dur = -1.0, ease::Fn curve = nullptr) noexcept {
        const double d = dur < 0.0 ? default_dur_ : dur;
        value_.set_target(target, d, curve);
        moving_ = true;
    }

    // Retarget a spring (mode must be spring). Velocity is preserved.
    void spring_to(T target) noexcept {
        value_.set_target(target);
        moving_ = true;
    }

    // Jump to a value instantly, killing any motion.
    void snap(T v) noexcept {
        value_.reset_to(v);
        moving_ = false;
    }

    // ── reading ────────────────────────────────────────────────────────
    // THE call. Ticks against the shared clock and, while moving, asks the
    // loop for another frame. Returns the current value. Idempotent within
    // a frame (the clock caches dt), so reading the same Motion twice in
    // one build() does not double-step it.
    [[nodiscard]] T get() const { return get_on(default_clock()); }

    // Tick against an explicit clock (tests, off-screen scratch timelines).
    [[nodiscard]] T get_on(Clock& clk) const {
        if (moving_) {
            value_.tick(clk.dt());
            if (value_.done()) {
                moving_ = false;
            } else {
                detail::request_frame();
            }
        }
        return value_.value();
    }

    // Peek without ticking or requesting a frame.
    [[nodiscard]] T peek() const noexcept { return value_.value(); }
    [[nodiscard]] T target() const noexcept { return value_.target(); }
    [[nodiscard]] bool moving() const noexcept { return moving_; }

private:
    mutable Animated<T> value_{};
    double              default_dur_ = 0.25;
    ease::Fn            curve_       = ease::out_cubic;
    bool                spring_      = false;
    mutable bool        moving_      = false;
};

// ============================================================================
// loop / pulse / ping_pong — perpetual phase helpers
// ============================================================================
// Many "alive" effects (blink, breathe, shimmer, caret pulse) don't have a
// target — they cycle forever off wall-clock. These free functions turn the
// clock's now_ms() into a normalized phase, and request a frame so the cycle
// keeps ticking. Pure reads of the clock; hold no state.

// Sawtooth phase in [0,1) over `period_ms`. Frame-requesting.
[[nodiscard]] inline double loop_phase(double period_ms,
                                       Clock& clk = default_clock()) noexcept {
    detail::request_frame();
    if (period_ms <= 0.0) return 0.0;
    const double t = static_cast<double>(clk.now_ms());
    double p = std::fmod(t, period_ms) / period_ms;
    if (p < 0.0) p += 1.0;
    return p;
}

// Eased triangle (ping-pong) in [0,1] over `period_ms`, peaking at the
// half-period. Smoothstep-shaped so it decelerates at both ends — the
// natural "breathing" feel. Frame-requesting.
[[nodiscard]] inline double pulse(double period_ms,
                                  Clock& clk = default_clock()) noexcept {
    const double p = loop_phase(period_ms, clk);   // 0..1 sawtooth
    const double tri = p < 0.5 ? (p * 2.0) : (2.0 - p * 2.0);  // 0→1→0
    return ease::smoothstep(tri);
}

// Convenience: pulse between two values of an lerp-able T.
template <typename T>
[[nodiscard]] inline T pulse_between(T a, T b, double period_ms,
                                     Clock& clk = default_clock()) noexcept {
    return lerp(a, b, pulse(period_ms, clk));
}

// ============================================================================
// Timeline — sequential / parallel choreography
// ============================================================================
// For multi-step animation that a single Motion can't express: "fade in,
// THEN slide up, THEN (after 200ms) pulse" or "do A and B in parallel."
//
// A Timeline is a list of TRACKS; each track is a list of KEYFRAMES with
// absolute start times and durations. You sample the whole timeline at a
// playhead (driven by the clock) and read each track's current value. The
// timeline reports done() when the playhead passes the last keyframe; until
// then sampling requests frames.
//
// This is the escape hatch for arbitrarily complex choreography while
// staying declarative — you describe the schedule once, then just sample.
//
//   anim::Timeline tl;
//   auto opacity = tl.track(0.0);
//   opacity.hold(0.0, 0.1)                 // wait 100ms
//          .to(1.0, 0.3, ease::out_cubic); // then fade in over 300ms
//   auto y = tl.track(8.0);
//   y.at(0.1).to(0.0, 0.4, ease::out_back);// slide up, overlapping the fade
//   ...
//   tl.play();                  // start (or restart) the playhead
//   // in build():
//   tl.sample();                // advance playhead off the clock
//   double op = opacity.value();
//   double yy = y.value();
//
// Tracks are value handles into the timeline's storage; cheap to copy.
class Timeline {
public:
    // A single segment of a track: from `start_s` for `dur_s`, lerp v0→v1.
    struct Key {
        double   start_s = 0.0;
        double   dur_s   = 0.0;
        double   v0      = 0.0;
        double   v1      = 0.0;
        ease::Fn curve   = ease::out_cubic;
    };

private:
    struct Data {
        double           init_v   = 0.0;
        double           last_v   = 0.0;  // build-time cursor value
        double           cursor_s = 0.0;  // build-time time cursor
        std::vector<Key> keys;
    };

public:
    class Track {
    public:
        Track(Timeline* tl, std::size_t idx) : tl_(tl), idx_(idx) {}

        // Hold the current value for `secs` (a gap before the next move).
        Track& hold(double value, double secs) {
            auto& t = data();
            t.cursor_s += (secs < 0.0 ? 0.0 : secs);
            t.last_v = value;
            return *this;
        }

        // Move to `target` over `secs` starting at the track's cursor.
        Track& to(double target, double secs, ease::Fn curve = ease::out_cubic) {
            auto& t = data();
            t.keys.push_back(Key{
                .start_s = t.cursor_s,
                .dur_s   = secs < 0.0 ? 0.0 : secs,
                .v0      = t.last_v,
                .v1      = target,
                .curve   = curve ? curve : ease::linear,
            });
            t.cursor_s += (secs < 0.0 ? 0.0 : secs);
            t.last_v = target;
            if (t.cursor_s > tl_->duration_s_) tl_->duration_s_ = t.cursor_s;
            return *this;
        }

        // Reposition the track cursor to an ABSOLUTE time (for overlapping
        // tracks that should start partway through another's motion).
        Track& at(double abs_secs) {
            data().cursor_s = abs_secs < 0.0 ? 0.0 : abs_secs;
            return *this;
        }

        // Current value at the timeline playhead.
        [[nodiscard]] double value() const {
            const auto& t = data();
            const double now = tl_->playhead_s_;
            // Before the first key: initial value.
            if (t.keys.empty()) return t.init_v;
            double v = t.keys.front().v0;
            for (const auto& k : t.keys) {
                if (now < k.start_s) break;          // not started → keep prev
                if (now >= k.start_s + k.dur_s) {     // finished → end value
                    v = k.v1;
                    continue;
                }
                const double raw = k.dur_s <= 0.0 ? 1.0
                                 : (now - k.start_s) / k.dur_s;
                v = lerp(k.v0, k.v1, k.curve(raw));
                return v;                             // mid-segment, done
            }
            return v;
        }

    private:
        [[nodiscard]] Data& data() const { return tl_->tracks_[idx_]; }
        Timeline*   tl_;
        std::size_t idx_;
        friend class Timeline;
    };

    // Create a track with an initial value.
    [[nodiscard]] Track track(double initial = 0.0) {
        tracks_.push_back(Data{.init_v = initial, .last_v = initial});
        return Track{this, tracks_.size() - 1};
    }

    // Start / restart the playhead from t=0.
    void play() { playing_ = true; playhead_s_ = 0.0; }
    void stop() { playing_ = false; }
    void seek(double secs) { playhead_s_ = secs; }

    // Advance the playhead off the clock and request a frame while running.
    // Call once per build(). Returns done() for convenience.
    bool sample(Clock& clk = default_clock()) {
        if (playing_ && !done()) {
            playhead_s_ += clk.dt();
            if (playhead_s_ >= duration_s_) {
                playhead_s_ = duration_s_;
                if (looping_) { playhead_s_ = 0.0; }
                else          { playing_ = false; }
            }
            detail::request_frame();
        }
        return done();
    }

    void set_loop(bool on) { looping_ = on; }
    [[nodiscard]] bool done() const {
        return !looping_ && playhead_s_ >= duration_s_;
    }
    [[nodiscard]] double playhead() const { return playhead_s_; }
    [[nodiscard]] double duration() const { return duration_s_; }

private:
    std::vector<Data> tracks_;
    double            duration_s_ = 0.0;
    double            playhead_s_ = 0.0;
    bool              playing_    = false;
    bool              looping_    = false;
    friend class Track;
};

// ============================================================================
// Stagger — index-phased fan-out
// ============================================================================
// The canonical "list items cascade in" / "menu opens row by row" motion:
// the SAME animation, applied to N items, each delayed by `step_s * index`.
// Given the timeline-global elapsed seconds, stagger_progress() returns the
// eased [0,1] progress for item `i` — clamp-safe at both ends.
//
//   double e = mount_clock_s;              // seconds since the list mounted
//   for (int i = 0; i < n; ++i) {
//       double p = anim::stagger_progress(e, i, 0.05, 0.30);
//       rows.push_back(row(i) | opacity(p) | translate_y((1-p)*6));
//   }
//
// step_s   per-item delay; item i starts at i*step_s.
// dur_s    each item's own animation length.
[[nodiscard]] inline double stagger_progress(
    double elapsed_s, int index, double step_s, double dur_s,
    ease::Fn curve = ease::out_cubic) noexcept {
    const double start = static_cast<double>(index) * step_s;
    const double raw   = dur_s <= 0.0 ? 1.0 : (elapsed_s - start) / dur_s;
    return (curve ? curve : ease::linear)(ease::clamp01(raw));
}

// True once every item in an N-stagger has finished (so the host can stop
// requesting frames).
[[nodiscard]] inline bool stagger_done(double elapsed_s, int count,
                                       double step_s, double dur_s) noexcept {
    if (count <= 0) return true;
    return elapsed_s >= static_cast<double>(count - 1) * step_s + dur_s;
}

} // namespace maya::anim
