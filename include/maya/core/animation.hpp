#pragma once
// maya::core::animation — Time-driven value interpolation
//
// A small, allocation-free animation runtime: drive a numeric value from a
// start to a target over wall-clock time, shaped by an easing curve OR by a
// critically-tunable spring. Everything is computed in `double` seconds and
// advanced explicitly via `tick(dt)` — there is no hidden clock, no thread,
// no per-frame allocation. The host owns the frame loop; maya just integrates.
//
// Two flavours:
//
//   Tween<T>     — fixed-duration A→B interpolation through an Easing curve.
//                  Deterministic, ends exactly at `target` after `duration`.
//
//   Spring<T>    — physically-modelled motion (stiffness/damping). Retargets
//                  mid-flight without a discontinuity (velocity is preserved),
//                  which is what makes spring motion feel "alive". Settles
//                  asymptotically; `done()` reports rest within an epsilon.
//
//   Animated<T>  — ergonomic wrapper that holds either a Tween or a Spring,
//                  exposes `value()`, `set_target()`, `tick(dt)`, `done()`.
//
// Easing functions are `constexpr` (usable at compile time, and trivially
// inlined at run time). Spring parameters are validated with `consteval`
// factories so an over-/under-damped misconfiguration is caught at the call
// site instead of producing NaNs at run time.
//
// Design notes:
//   * No heap. Tween/Spring/Animated are trivially copyable value types.
//   * `T` may be any type that supports scalar lerp via operator+/-/*. The
//     library ships specialisations for float/double and for maya RGB colour
//     (componentwise, gamma-naive — adequate for UI fades).
//   * tick(dt) is O(1) and branch-light; safe to call thousands of times per
//     frame across many Animated<> instances without measurable cost.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "../style/color.hpp"

namespace maya::anim {

// ============================================================================
// constexpr math — std::sqrt / std::abs(double) are NOT standard-constexpr.
// ============================================================================
// libstdc++/GCC offer them as a non-conforming extension, but libc++ (clang)
// and MSVC correctly reject them in a constant-expression context. The spring
// presets need a sqrt at consteval time, so we ship our own constexpr ones.
// (At run time the compiler folds these to the same code as the intrinsics.)
namespace cmath {

[[nodiscard]] constexpr double c_abs(double x) noexcept {
    return x < 0.0 ? -x : x;
}

// Newton–Raphson sqrt. Converges quadratically; ~30 iters is overkill but
// keeps it exact across the whole positive double range at compile time.
[[nodiscard]] constexpr double c_sqrt(double x) noexcept {
    if (x < 0.0) return 0.0;          // domain guard (no NaN at constexpr)
    if (x == 0.0 || x == 1.0) return x;
    double guess = x < 1.0 ? x : x / 2.0;
    for (int i = 0; i < 40; ++i) {
        const double next = 0.5 * (guess + x / guess);
        if (c_abs(next - guess) < 1e-15 * (next < 1.0 ? 1.0 : next)) {
            guess = next;
            break;
        }
        guess = next;
    }
    return guess;
}

} // namespace cmath

// ============================================================================
// Easing — constexpr curve functions on a normalised parameter t ∈ [0,1]
// ============================================================================
// Each returns a shaped progress in [0,1]. They are pure and constexpr, so
// `static_assert(ease::in_out_cubic(0.5) == 0.5)` works at compile time.

namespace ease {

[[nodiscard]] constexpr double clamp01(double t) noexcept {
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

[[nodiscard]] constexpr double linear(double t) noexcept { return clamp01(t); }

[[nodiscard]] constexpr double in_quad(double t) noexcept {
    t = clamp01(t);
    return t * t;
}
[[nodiscard]] constexpr double out_quad(double t) noexcept {
    t = clamp01(t);
    return t * (2.0 - t);
}
[[nodiscard]] constexpr double in_out_quad(double t) noexcept {
    t = clamp01(t);
    return t < 0.5 ? 2.0 * t * t : 1.0 - 0.5 * (2.0 * t - 2.0) * (2.0 * t - 2.0);
}

[[nodiscard]] constexpr double in_cubic(double t) noexcept {
    t = clamp01(t);
    return t * t * t;
}
[[nodiscard]] constexpr double out_cubic(double t) noexcept {
    t = clamp01(t);
    const double f = t - 1.0;
    return f * f * f + 1.0;
}
[[nodiscard]] constexpr double in_out_cubic(double t) noexcept {
    t = clamp01(t);
    if (t < 0.5) return 4.0 * t * t * t;
    const double f = 2.0 * t - 2.0;
    return 0.5 * f * f * f + 1.0;
}

// Quintic — snappier ends, used for "settle" feel.
[[nodiscard]] constexpr double in_out_quint(double t) noexcept {
    t = clamp01(t);
    if (t < 0.5) return 16.0 * t * t * t * t * t;
    const double f = 2.0 * t - 2.0;
    return 0.5 * f * f * f * f * f + 1.0;
}

// Smoothstep / smootherstep (Ken Perlin). C1 / C2 continuous; no overshoot.
[[nodiscard]] constexpr double smoothstep(double t) noexcept {
    t = clamp01(t);
    return t * t * (3.0 - 2.0 * t);
}
[[nodiscard]] constexpr double smootherstep(double t) noexcept {
    t = clamp01(t);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// Back — anticipation / overshoot. in_back dips below 0 at the start
// ("wind-up"); out_back overshoots past 1 near the end then settles
// ("snap into place"). The classic UI "pop" curve. s = 1.70158 gives the
// standard ~10% overshoot. NOTE: range escapes [0,1] by design — callers
// lerping positions get the overshoot; callers lerping a CLAMPED quantity
// (alpha, a colour mix) should prefer out_cubic.
[[nodiscard]] constexpr double in_back(double t) noexcept {
    t = clamp01(t);
    constexpr double s = 1.70158;
    return t * t * ((s + 1.0) * t - s);
}
[[nodiscard]] constexpr double out_back(double t) noexcept {
    t = clamp01(t);
    constexpr double s = 1.70158;
    const double f = t - 1.0;
    return f * f * ((s + 1.0) * f + s) + 1.0;
}

// Easing function pointer type — lets widgets accept a curve as a parameter.
using Fn = double (*)(double) noexcept;

} // namespace ease

// ============================================================================
// Lerp — generic linear interpolation primitive
// ============================================================================
// Override `lerp()` for custom T by ADL or by adding an overload here.

template <typename T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T lerp(T a, T b, double t) noexcept {
    return static_cast<T>(static_cast<double>(a) +
                          (static_cast<double>(b) - static_cast<double>(a)) * t);
}

// maya truecolor — componentwise lerp on the RGB channels. Rounds to nearest.
// Only meaningful for Rgb-kind colours; named/indexed degrade to their RGB
// projection via the accessors, which is adequate for UI fades.
[[nodiscard]] inline Color lerp(Color a, Color b, double t) noexcept {
    auto mix = [t](uint8_t x, uint8_t y) -> uint8_t {
        const double v = static_cast<double>(x) +
                         (static_cast<double>(y) - static_cast<double>(x)) * t;
        return static_cast<uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
    };
    return Color::rgb(mix(a.r(), b.r()), mix(a.g(), b.g()), mix(a.b(), b.b()));
}

// ============================================================================
// Tween<T> — fixed-duration eased interpolation
// ============================================================================

template <typename T>
class Tween {
    T          from_{};
    T          to_{};
    double     duration_ = 0.0;   // seconds; 0 ⇒ instantaneous
    double     elapsed_  = 0.0;
    ease::Fn   ease_     = ease::in_out_cubic;

public:
    constexpr Tween() = default;

    constexpr Tween(T from, T to, double duration_secs,
                    ease::Fn curve = ease::in_out_cubic) noexcept
        : from_(from), to_(to),
          duration_(duration_secs < 0.0 ? 0.0 : duration_secs),
          ease_(curve ? curve : ease::linear) {}

    // Advance by dt seconds. Returns the current (post-tick) value.
    constexpr T tick(double dt) noexcept {
        if (dt > 0.0) elapsed_ += dt;
        return value();
    }

    [[nodiscard]] constexpr T value() const noexcept {
        if (duration_ <= 0.0) return to_;
        const double raw = elapsed_ / duration_;
        const double t   = ease_(raw >= 1.0 ? 1.0 : raw);
        return lerp(from_, to_, t);
    }

    [[nodiscard]] constexpr bool done() const noexcept {
        return elapsed_ >= duration_;
    }

    // Retarget: continue from the CURRENT value toward a new target over a
    // fresh duration. Preserves visual continuity (no jump).
    constexpr void retarget(T new_to, double duration_secs,
                            ease::Fn curve = nullptr) noexcept {
        from_     = value();
        to_       = new_to;
        duration_ = duration_secs < 0.0 ? 0.0 : duration_secs;
        elapsed_  = 0.0;
        if (curve) ease_ = curve;
    }

    constexpr void reset_to(T v) noexcept {
        from_ = to_ = v;
        elapsed_ = duration_ = 0.0;
    }

    [[nodiscard]] constexpr T   target()   const noexcept { return to_; }
    [[nodiscard]] constexpr double progress() const noexcept {
        return duration_ <= 0.0 ? 1.0
                                : ease::clamp01(elapsed_ / duration_);
    }
};

// ============================================================================
// SpringParams — validated spring configuration
// ============================================================================
// stiffness (k) pulls toward target; damping (c) bleeds velocity. The pair
// determines the response: under-damped overshoots & oscillates, critically
// damped reaches rest fastest without overshoot, over-damped crawls in.
//
// Use the consteval factories so a degenerate config is a compile error.

struct SpringParams {
    double stiffness = 170.0;  // k  — typical UI: 100..300
    double damping   = 20.0;   // c
    double mass      = 1.0;    // m
    double rest_eps  = 0.001;  // |x-target| & |v| below this ⇒ settled

    // Damping ratio ζ = c / (2·√(k·m)). ζ<1 underdamped, =1 critical, >1 over.
    [[nodiscard]] constexpr double zeta() const noexcept {
        const double denom = 2.0 * cmath::c_sqrt(stiffness * mass);
        return denom > 0.0 ? damping / denom : 0.0;
    }
};

namespace spring_presets {

// Build a spring from intuitive "stiffness + damping ratio" knobs and validate
// at compile time. ζ ∈ (0, 2]; stiffness > 0; mass > 0.
[[nodiscard]] consteval SpringParams make(double stiffness, double zeta,
                                          double mass = 1.0,
                                          double rest_eps = 0.001) {
    if (!(stiffness > 0.0)) throw "spring: stiffness must be > 0";
    if (!(mass > 0.0))      throw "spring: mass must be > 0";
    if (!(zeta > 0.0))      throw "spring: damping ratio must be > 0";
    if (zeta > 4.0)         throw "spring: damping ratio absurdly high (>4)";
    if (!(rest_eps > 0.0))  throw "spring: rest_eps must be > 0";
    const double c = zeta * 2.0 * cmath::c_sqrt(stiffness * mass);
    return SpringParams{stiffness, c, mass, rest_eps};
}

// Hand-tuned presets (Framer-Motion-ish feel).
inline constexpr SpringParams gentle   = make(120.0, 0.90);
inline constexpr SpringParams snappy    = make(210.0, 0.80);
inline constexpr SpringParams wobbly    = make(180.0, 0.45);
inline constexpr SpringParams stiff     = make(300.0, 1.00);
inline constexpr SpringParams molasses  = make(60.0,  1.20);

} // namespace spring_presets

// ============================================================================
// Spring<T> — semi-implicit Euler spring integrator
// ============================================================================
// Operates on the scalar projection of T (via lerp). For arithmetic T this is
// the value itself; for colour it springs the 0→1 mix parameter between the
// last-set base and the target, which keeps channels coupled (no hue drift).

template <typename T>
class Spring {
    T            base_{};    // value the spring departed from
    T            target_{};  // value the spring pulls toward
    double       x_  = 0.0;  // position along base→target (0..1, can overshoot)
    double       v_  = 0.0;  // velocity in the same 1-D space
    SpringParams p_{};

public:
    constexpr Spring() = default;

    explicit constexpr Spring(T initial, SpringParams params = {}) noexcept
        : base_(initial), target_(initial), p_(params) {}

    // Semi-implicit (symplectic) Euler — stable for stiff springs at the dt
    // values a 60 fps loop produces. Sub-steps when dt is large to keep the
    // integration well-conditioned after a dropped frame.
    constexpr T tick(double dt) noexcept {
        if (dt <= 0.0) return value();
        // Cap a catch-up frame and sub-step at ≤ 1/120 s for stiff springs.
        if (dt > 0.25) dt = 0.25;
        constexpr double kMaxStep = 1.0 / 120.0;
        int steps = static_cast<int>(dt / kMaxStep) + 1;
        const double h = dt / static_cast<double>(steps);
        for (int i = 0; i < steps; ++i) {
            const double a = (-p_.stiffness * (x_ - 1.0) - p_.damping * v_) / p_.mass;
            v_ += a * h;
            x_ += v_ * h;
        }
        if (done()) { x_ = 1.0; v_ = 0.0; }
        return value();
    }

    [[nodiscard]] constexpr T value() const noexcept {
        return lerp(base_, target_, x_);
    }

    [[nodiscard]] constexpr bool done() const noexcept {
        return cmath::c_abs(x_ - 1.0) < p_.rest_eps && cmath::c_abs(v_) < p_.rest_eps;
    }

    // Retarget mid-flight WITHOUT killing momentum. The current value becomes
    // the new base; the velocity (in value-units/sec) is reprojected onto the
    // new base→target span so motion stays continuous.
    constexpr void set_target(T new_target) noexcept {
        const T   cur          = value();
        const double span_old  = scalar_span(base_, target_);
        const double vel_units = v_ * span_old;          // value-units / sec
        base_   = cur;
        target_ = new_target;
        const double span_new  = scalar_span(base_, target_);
        x_ = 0.0;
        v_ = span_new != 0.0 ? vel_units / span_new : 0.0;
        if (span_new == 0.0) { x_ = 1.0; v_ = 0.0; }
    }

    constexpr void reset_to(T v) noexcept {
        base_ = target_ = v;
        x_ = 1.0; v_ = 0.0;
    }

    constexpr void set_params(SpringParams p) noexcept { p_ = p; }

    [[nodiscard]] constexpr T            target()   const noexcept { return target_; }
    [[nodiscard]] constexpr double       velocity() const noexcept { return v_; }
    [[nodiscard]] constexpr SpringParams params()   const noexcept { return p_; }

private:
    // Magnitude of the base→target gap projected to scalar, for momentum
    // reprojection. Arithmetic: |b-a|. Colour: max channel delta.
    [[nodiscard]] static constexpr double scalar_span(T a, T b) noexcept {
        if constexpr (std::is_arithmetic_v<T>) {
            return cmath::c_abs(static_cast<double>(b) - static_cast<double>(a));
        } else {
            return 1.0; // non-arithmetic springs use a unit param space
        }
    }
};

// Colour-specialised scalar_span (max channel delta) lives as an overload so
// momentum reprojection on Color stays continuous.
template <>
inline constexpr double Spring<Color>::scalar_span(Color a, Color b) noexcept {
    const int dr = std::abs(int(a.r()) - int(b.r()));
    const int dg = std::abs(int(a.g()) - int(b.g()));
    const int db = std::abs(int(a.b()) - int(b.b()));
    return static_cast<double>(std::max({dr, dg, db})) / 255.0;
}

// ============================================================================
// Animated<T> — one value, either tweened or sprung
// ============================================================================
// The public ergonomic type. Construct with `Animated<T>::tween(...)` or
// `Animated<T>::spring(...)`. Call `tick(dt)` each frame; `done()` tells the
// host whether to keep requesting animation frames.

template <typename T>
class Animated {
    enum class Mode : uint8_t { Tween, Spring };
    Mode      mode_ = Mode::Tween;
    Tween<T>  tween_{};
    Spring<T> spring_{};

public:
    constexpr Animated() = default;

    [[nodiscard]] static constexpr Animated tween(
        T from, T to, double duration_secs,
        ease::Fn curve = ease::in_out_cubic) noexcept {
        Animated a;
        a.mode_  = Mode::Tween;
        a.tween_ = Tween<T>(from, to, duration_secs, curve);
        return a;
    }

    [[nodiscard]] static constexpr Animated spring(
        T initial, SpringParams params = {}) noexcept {
        Animated a;
        a.mode_   = Mode::Spring;
        a.spring_ = Spring<T>(initial, params);
        return a;
    }

    constexpr T tick(double dt) noexcept {
        return mode_ == Mode::Tween ? tween_.tick(dt) : spring_.tick(dt);
    }

    [[nodiscard]] constexpr T value() const noexcept {
        return mode_ == Mode::Tween ? tween_.value() : spring_.value();
    }

    [[nodiscard]] constexpr bool done() const noexcept {
        return mode_ == Mode::Tween ? tween_.done() : spring_.done();
    }

    [[nodiscard]] constexpr T target() const noexcept {
        return mode_ == Mode::Tween ? tween_.target() : spring_.target();
    }

    // Retarget. For a tween, requires a duration; for a spring it preserves
    // momentum. The duration arg is ignored in spring mode.
    constexpr void set_target(T new_target, double duration_secs = 0.25,
                              ease::Fn curve = nullptr) noexcept {
        if (mode_ == Mode::Tween) tween_.retarget(new_target, duration_secs, curve);
        else                      spring_.set_target(new_target);
    }

    constexpr void reset_to(T v) noexcept {
        if (mode_ == Mode::Tween) tween_.reset_to(v);
        else                      spring_.reset_to(v);
    }

    [[nodiscard]] constexpr bool is_spring() const noexcept {
        return mode_ == Mode::Spring;
    }
};

// ============================================================================
// RateCursor — constant-rate typewriter integrator with burst-drain + ramp
// ============================================================================
// A monotone cursor (think "codepoints revealed so far") that chases a
// moving target ("codepoints available") at a controlled RATE rather than a
// fixed duration. This is the shape a typewriter reveal needs and that
// neither Tween (duration-based) nor Spring (asymptotic, overshoots) can
// express:
//
//   * floor_rate is a CEILING under normal flow — the cursor never walks
//     faster than this, so each unit takes its turn even when the target
//     grows one-unit-at-a-time (a slow byte feed). Without the ceiling a
//     backlog-proportional rate would snap to the edge instantly and the
//     reveal would be invisible.
//   * when the backlog exceeds drain_secs worth at floor_rate, the rate
//     accelerates to backlog/drain_secs so a burst clears within its
//     target window.
//   * an optional finalize ramp (set_deadline) guarantees the cursor
//     reaches the edge by a wall-clock deadline regardless of the above
//     (end-of-stream settle).
//
// Pure value type, no clock: the host passes elapsed seconds to tick().
// Mirrors the reveal_cp_ integrator in reveal_fx.cpp exactly so that math
// lives in one tested place.
class RateCursor {
    double pos_        = 0.0;   // current cursor position (units)
    double floor_rate_ = 30.0;  // units/sec ceiling under normal flow
    double drain_secs_ = 0.25;  // burst target: clear backlog within this
    double ramp_left_  = -1.0;  // seconds until finalize deadline; <0 = none
    double smooth_rate_ = -1.0; // low-passed rate (units/sec); <0 = unset
    // Velocity smoothing time-constant (seconds). The instantaneous rate
    // (max(backlog/drain, floor)) is proportional to backlog, and backlog
    // sawtooths because bytes arrive in spiky chunks — so the raw rate jumps
    // frame to frame, which the eye reads as the cursor SPRINTING then
    // IDLING ("bursts, not smooth"). Exponentially smoothing the rate toward
    // its target with this time-constant turns those steps into a continuous
    // glide: speed RAMPS into a burst and coasts back down instead of
    // teleporting. Small enough that catch-up is still prompt (the cursor
    // reaches the live edge within a few frames of a burst), large enough
    // that per-chunk spikes are averaged out.
    double smooth_tau_ = 0.18;

public:
    constexpr RateCursor() = default;
    constexpr RateCursor(double floor_rate, double drain_secs) noexcept
        : floor_rate_(floor_rate > 0.0 ? floor_rate : 1.0),
          drain_secs_(drain_secs > 0.0 ? drain_secs : 0.001) {}

    constexpr void set_pacing(double floor_rate, double drain_secs) noexcept {
        if (floor_rate > 0.0) floor_rate_ = floor_rate;
        if (drain_secs > 0.0) drain_secs_ = drain_secs;
    }

    // Tune the velocity-smoothing time-constant (seconds). 0 disables
    // smoothing (raw backlog-proportional rate — the old bursty behaviour).
    constexpr void set_smoothing(double tau_secs) noexcept {
        smooth_tau_ = tau_secs > 0.0 ? tau_secs : 0.0;
    }

    // Hard-set the cursor (e.g. snap forward past already-committed units,
    // or reset on a content rollback). Never moves the cursor backward via
    // tick(); this is the explicit override.
    constexpr void set_pos(double p) noexcept { pos_ = p < 0.0 ? 0.0 : p; }
    [[nodiscard]] constexpr double pos() const noexcept { return pos_; }

    // Ensure the cursor is at least `floor` (snap-to-committed). Monotone.
    constexpr void advance_floor(double floor) noexcept {
        if (pos_ < floor) pos_ = floor;
    }

    // Arm a finalize ramp: reach `target` within `secs` no matter what.
    constexpr void set_deadline(double secs) noexcept { ramp_left_ = secs; }
    constexpr void clear_deadline() noexcept { ramp_left_ = -1.0; }
    [[nodiscard]] constexpr bool ramping() const noexcept {
        return ramp_left_ >= 0.0;
    }

    // Advance toward `target` by `dt` seconds. Returns the new position.
    // `dt` should be pre-clamped by the host to a sane frame budget
    // (reveal_fx clamps to 0.120 s) so a long stall doesn't teleport the
    // cursor on the first frame back.
    //
    // Ramp semantics mirror reveal_fx exactly: the deadline test uses the
    // time remaining AS OF THIS FRAME's start (ramp_left_ before dt is
    // consumed). The host re-arms the deadline each frame via
    // set_deadline(remaining), so ramp_left_ already reflects the current
    // frame's remaining budget; we read it, decide the rate, THEN consume
    // dt for the next frame's accounting.
    constexpr double tick(double target, double dt) noexcept {
        if (dt < 0.0) dt = 0.0;

        const double backlog = target - pos_;
        if (backlog <= 0.0) {
            pos_ = target;
            if (ramp_left_ >= 0.0) ramp_left_ -= dt;
            return pos_;
        }

        const double burst = backlog / drain_secs_;
        double rate = burst > floor_rate_ ? burst : floor_rate_;

        // ── Velocity smoothing ──
        // Low-pass the rate toward its backlog-driven target so a chunky
        // arrival pattern produces a continuous glide rather than a
        // sprint/idle sawtooth. Applied BEFORE the ramp override so a
        // finalize deadline still snaps exactly (the ramp must hit its
        // target regardless of the smoothed velocity). The smoothed rate is
        // also floored at floor_rate_ so smoothing never drags the steady
        // typing speed below the floor.
        if (smooth_tau_ > 0.0) {
            if (smooth_rate_ < 0.0) {
                smooth_rate_ = rate;          // first frame: seed, no lag
            } else {
                // Exponential approach: alpha = 1 - e^(-dt/tau), but the
                // cheap/stable first-order form (dt/(dt+tau)) is plenty for
                // UI and stays constexpr-friendly without <cmath>.
                const double alpha = dt / (dt + smooth_tau_);
                smooth_rate_ += (rate - smooth_rate_) * alpha;
            }
            rate = smooth_rate_ < floor_rate_ ? floor_rate_ : smooth_rate_;
        }

        if (ramp_left_ >= 0.0) {
            // Finalize ramp: if at/past the deadline, snap (cover the whole
            // backlog this frame); else ensure the rate is at least enough
            // to cover the backlog in the time left. Bypasses smoothing —
            // the deadline is a hard guarantee.
            if (ramp_left_ <= 0.0) {
                rate = backlog / (dt > 0.001 ? dt : 0.001);
            } else {
                const double ramp = backlog / ramp_left_;
                if (ramp > rate) rate = ramp;
            }
            ramp_left_ -= dt;
            smooth_rate_ = rate;   // re-seed so post-ramp doesn't lurch
        }

        // Never overshoot the live edge: a smoothed/ramped rate could carry
        // the cursor past `target` on a frame where backlog is tiny.
        const double max_rate = backlog / (dt > 1e-9 ? dt : 1e-9);
        if (rate > max_rate) rate = max_rate;

        pos_ += rate * dt;
        if (pos_ > target) pos_ = target;
        return pos_;
    }

    constexpr void reset() noexcept {
        pos_ = 0.0; ramp_left_ = -1.0; smooth_rate_ = -1.0;
    }
};

} // namespace maya::anim
