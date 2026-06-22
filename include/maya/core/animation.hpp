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
        const double denom = 2.0 * std::sqrt(stiffness * mass);
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
    const double c = zeta * 2.0 * std::sqrt(stiffness * mass);
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
        return std::abs(x_ - 1.0) < p_.rest_eps && std::abs(v_) < p_.rest_eps;
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
            return std::abs(static_cast<double>(b) - static_cast<double>(a));
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

} // namespace maya::anim
