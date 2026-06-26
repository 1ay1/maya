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
    // Use the constexpr-safe cmath::c_abs (std::abs(int) is not guaranteed
    // constexpr, and MSVC rejects the whole function as never-constant).
    const double dr = cmath::c_abs(double(int(a.r()) - int(b.r())));
    const double dg = cmath::c_abs(double(int(a.g()) - int(b.g())));
    const double db = cmath::c_abs(double(int(a.b()) - int(b.b())));
    const double m = (dr > dg ? dr : dg);
    return (db > m ? db : m) / 255.0;
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
// express.
//
// ── DESIGN: constant-glide, buffer-absorbed (the ChatGPT / Vercel model) ──
//
// The lesson every polished streaming UI converges on (Vercel AI SDK's
// smoothStream, the Upstash production hook, ChatGPT, Claude.ai): DECOUPLE
// the visual speed from the network rhythm. Bytes arrive from the wire in
// spiky bursts; if the cursor speed tracks the backlog (even a smoothed
// track) it inherits that spikiness and reads as "bursts, not a glide". The
// fix is to drain the buffer at a near-CONSTANT speed and let the buffer
// itself absorb the jitter:
//
//   * base_rate_ is the STEADY glide speed (units/sec) — the speed the
//     cursor actually moves at almost all the time, like Vercel's fixed
//     delayInMs or Upstash's constant 200 cps. NOT a floor, not a ceiling:
//     the cruising speed.
//   * The base auto-tunes SLOWLY toward whatever speed would clear the
//     current backlog over a comfortable window (target_lead_secs), so a
//     consistently-fast model speeds the glide up and a slow one eases it
//     down — but the base changes over base_tau_ seconds, far slower than a
//     frame, so the change is imperceptible (no per-chunk speed steps).
//   * Instantaneous catch-up is BOUNDED: the actual per-frame rate is the
//     base plus a gentle proportional term, hard-capped at max_burst_mult_
//     × base. A huge backlog can never make the cursor sprint — it just
//     pins at the cap until the buffer drains, staying a glide.
//   * an optional finalize ramp (set_deadline) still guarantees the cursor
//     reaches the edge by a wall-clock deadline (end-of-stream settle),
//     bypassing the glide caps because that's a hard correctness guarantee.
//
// Pure value type, no clock: the host passes elapsed seconds to tick().
class RateCursor {
    double pos_        = 0.0;   // current cursor position (units)
    double floor_rate_ = 30.0;  // configured base glide speed (units/sec)
    double drain_secs_ = 0.25;  // target lead window: clear backlog over this
    double ramp_left_  = -1.0;  // seconds until finalize deadline; <0 = none
    double base_rate_  = -1.0;  // live auto-tuned glide speed; <0 = unset
    // How fast the auto-tuned base may itself change (seconds). Large => the
    // cruising speed drifts very slowly, so the user never perceives a speed
    // STEP at a chunk boundary — the whole point of the constant-glide model.
    double base_tau_   = 0.6;
    // Hard ceiling on the instantaneous rate as a multiple of the base. The
    // catch-up term can lift the rate toward this on a backlog spike, but
    // never past it, so the cursor accelerates GENTLY and predictably
    // instead of teleporting. ~1.8× keeps even a big burst feeling like a
    // glide that briefly leans forward.
    double max_burst_mult_ = 1.8;
    // Hard ceiling on the AUTO-TUNED CRUISE speed, as a multiple of
    // floor_rate_. This is the load-bearing knob for the constant-glide
    // model. Without it, the auto-tune target (backlog / drain_secs_) runs
    // away whenever the model streams faster than floor_rate_: on a long,
    // fast reply the backlog grows, base_rate_ chases it up to thousands of
    // cp/s, and the "typewriter" turns into an instant paste — the
    // "bursting in long turns" symptom. Polished chat UIs (ChatGPT, Claude,
    // Vercel smoothStream) hold a CONSTANT visual rate and let the buffer
    // absorb the wire's burstiness; they may glide a touch faster on a long
    // reply but never sprint to an unreadable speed. Clamping the cruise
    // target to floor_rate_ * this multiple reproduces that: the cursor
    // cruises at floor_rate_ normally and tops out at
    // floor_rate_ * cruise_ceiling_mult_ on a sustained fast stream, staying
    // readable. A genuinely huge backlog (model dumped the whole reply at
    // once) is NOT chased here — the finalize ramp clears the remainder at
    // end-of-stream; mid-stream the cursor just stays at the ceiling and
    // glides. ~2.5× floor (e.g. 90 → 225 cp/s) is a brisk-but-readable cap.
    double cruise_ceiling_mult_ = 2.5;
    // Hard ceiling on how far the cursor may fall BEHIND the live edge, in
    // seconds of cruise-ceiling reveal. The cruise/burst caps above bound the
    // cursor's SPEED — which means a model streaming faster than the ceiling
    // makes the cursor fall progressively further behind: the unrevealed
    // backlog grows without bound and the visible text lags the arrived text
    // by (backlog / ceiling) seconds. On a long, fast reply that lag climbs
    // to tens of seconds (a 10 KB turn took ~25 s to finish revealing) — the
    // "streaming sticks / crawls, then dumps at the end on a long turn"
    // symptom. Capping the SPEED without capping the LAG is the bug. So also
    // bound the lag: the cursor never rides more than
    // floor_rate_ * cruise_ceiling_mult_ * max_lag_secs_ codepoints behind
    // the edge. Within that window it glides at the smooth cruise; once the
    // wire outruns it past the window the cursor pins to the window boundary
    // and thereafter tracks the wire rate (revealing a constant-width
    // scramble trail), so the visible reveal stays within ~max_lag_secs_ of
    // what actually arrived no matter how long or fast the turn is. Set <= 0
    // to disable (unbounded lag, the old behaviour).
    double max_lag_secs_ = 1.0;

public:
    constexpr RateCursor() = default;
    constexpr RateCursor(double floor_rate, double drain_secs) noexcept
        : floor_rate_(floor_rate > 0.0 ? floor_rate : 1.0),
          drain_secs_(drain_secs > 0.0 ? drain_secs : 0.001) {}

    constexpr void set_pacing(double floor_rate, double drain_secs) noexcept {
        if (floor_rate > 0.0) floor_rate_ = floor_rate;
        if (drain_secs > 0.0) drain_secs_ = drain_secs;
    }

    // Tune the glide shaping. base_tau: how slowly the cruising speed drifts
    // (bigger = steadier). max_burst_mult: instantaneous-rate ceiling as a
    // multiple of the base (bigger = catches up harder on a spike).
    // cruise_ceiling_mult: hard cap on the auto-tuned cruise speed as a
    // multiple of floor_rate_ (bigger = lets a long fast reply glide quicker;
    // keep it modest — this is what stops the long-turn burst). Pass a
    // non-positive value to leave any of them unchanged.
    constexpr void set_smoothing(double base_tau,
                                 double max_burst_mult = -1.0,
                                 double cruise_ceiling_mult = -1.0) noexcept {
        if (base_tau >= 0.0)            base_tau_            = base_tau;
        if (max_burst_mult > 0.0)       max_burst_mult_      = max_burst_mult;
        if (cruise_ceiling_mult > 0.0)  cruise_ceiling_mult_ = cruise_ceiling_mult;
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

        // ── Auto-tune the cruising speed (slowly, BOUNDED) ──
        // The "ideal" steady speed to hold the backlog at the target lead
        // window is backlog / drain_secs_. Blend the configured base
        // (floor_rate_) with that so a fast model glides a touch quicker and
        // a slow one eases down, but CLAMP the target to a readable ceiling
        // (floor_rate_ * cruise_ceiling_mult_). The clamp is the whole game:
        // an unclamped target chases the backlog to thousands of cp/s on a
        // long fast reply, turning the glide into an instant paste (the
        // "bursting in long turns" bug). With the clamp the cruise tops out
        // at a brisk-but-readable speed and the buffer absorbs the rest — the
        // constant-glide model every polished chat UI uses. Then drift the
        // LIVE base toward the (clamped) target over base_tau_ so the speed
        // never STEPS at a chunk boundary.
        const double cruise_ceiling = floor_rate_ * cruise_ceiling_mult_;
        const double want = backlog / drain_secs_;
        double base_target = want > floor_rate_ ? want : floor_rate_;
        if (base_target > cruise_ceiling) base_target = cruise_ceiling;
        if (base_rate_ < 0.0) {
            base_rate_ = base_target;            // first frame: seed, no lag
        } else if (base_tau_ > 0.0) {
            const double a = dt / (dt + base_tau_);
            base_rate_ += (base_target - base_rate_) * a;
        } else {
            base_rate_ = base_target;
        }
        if (base_rate_ < floor_rate_)    base_rate_ = floor_rate_;
        if (base_rate_ > cruise_ceiling) base_rate_ = cruise_ceiling;

        // ── Instantaneous rate: base + gentle, BOUNDED catch-up ──
        // A small proportional nudge leans the cursor forward when it's
        // behind, but the whole thing is hard-capped at max_burst_mult_ ×
        // base so a backlog spike can never make it sprint — it just pins at
        // the cap and glides until the buffer drains.
        double rate = base_rate_;
        const double lead = backlog - base_rate_ * drain_secs_;  // excess units
        if (lead > 0.0) rate += lead / drain_secs_;              // proportional
        const double cap = base_rate_ * max_burst_mult_;
        if (rate > cap) rate = cap;

        if (ramp_left_ >= 0.0) {
            // Finalize ramp: a HARD wall-clock guarantee that the cursor
            // reaches the edge by the deadline — bypasses the glide caps.
            if (ramp_left_ <= 0.0) {
                rate = backlog / (dt > 0.001 ? dt : 0.001);
            } else {
                const double ramp = backlog / ramp_left_;
                if (ramp > rate) rate = ramp;
            }
            ramp_left_ -= dt;
            base_rate_ = rate;     // re-seed so post-ramp doesn't lurch
        }

        // Never overshoot the live edge on a tiny-backlog frame.
        const double max_rate = backlog / (dt > 1e-9 ? dt : 1e-9);
        if (rate > max_rate) rate = max_rate;

        pos_ += rate * dt;
        if (pos_ > target) pos_ = target;

        // ── Hard lag ceiling: bound how far behind the edge we may ride ──
        // The speed caps above can leave the cursor permanently behind a
        // faster-than-ceiling wire, so the backlog (and the visible lag)
        // grows with the turn length. Clamp it: never trail the live edge by
        // more than max_lag_secs_ worth of cruise-ceiling reveal. This is a
        // forward-only move (monotone, never un-reveals), engages only once
        // the cursor has fallen behind the window, and then rides the
        // boundary frame-to-frame (revealing at the wire rate) instead of
        // snapping — so a fast/long turn keeps up with a fixed scramble trail
        // rather than crawling seconds behind. Short/slow replies never reach
        // the window, so their gentle typewriter glide is unchanged.
        if (max_lag_secs_ > 0.0) {
            const double max_lag =
                floor_rate_ * cruise_ceiling_mult_ * max_lag_secs_;
            if (target - pos_ > max_lag) pos_ = target - max_lag;
        }
        return pos_;
    }

    constexpr void reset() noexcept {
        pos_ = 0.0; ramp_left_ = -1.0; base_rate_ = -1.0;
    }
};

} // namespace maya::anim
