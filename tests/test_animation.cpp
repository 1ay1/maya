// Tests for the animation runtime: easing curves, Tween, Spring, Animated
#include <maya/maya.hpp>
#include <cassert>
#include <cmath>
#include <print>

using namespace maya;
using namespace maya::anim;

static bool approx(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) < eps;
}

// ── Easing curves are constexpr and well-behaved ────────────────────────────
void test_easing_constexpr() {
    std::println("--- test_easing_constexpr ---");
    // Endpoints pinned at 0 and 1 for every curve.
    static_assert(ease::linear(0.0) == 0.0 && ease::linear(1.0) == 1.0);
    static_assert(ease::in_quad(0.0) == 0.0 && ease::in_quad(1.0) == 1.0);
    static_assert(ease::out_quad(0.0) == 0.0 && ease::out_quad(1.0) == 1.0);
    static_assert(ease::in_cubic(0.0) == 0.0 && ease::in_cubic(1.0) == 1.0);
    static_assert(ease::out_cubic(0.0) == 0.0 && ease::out_cubic(1.0) == 1.0);
    static_assert(ease::in_out_cubic(0.0) == 0.0 && ease::in_out_cubic(1.0) == 1.0);
    static_assert(ease::smoothstep(0.0) == 0.0 && ease::smoothstep(1.0) == 1.0);
    static_assert(ease::smootherstep(0.0) == 0.0 && ease::smootherstep(1.0) == 1.0);

    // Symmetric curves cross 0.5 at the midpoint.
    static_assert(ease::linear(0.5) == 0.5);
    static_assert(ease::in_out_cubic(0.5) == 0.5);
    static_assert(ease::smoothstep(0.5) == 0.5);
    static_assert(ease::in_out_quint(0.5) == 0.5);

    // Clamped outside [0,1].
    assert(ease::out_cubic(-1.0) == 0.0);
    assert(ease::out_cubic(2.0) == 1.0);

    // Monotonic increasing across the unit interval.
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
        double v = ease::in_out_cubic(i / 100.0);
        assert(v >= prev - 1e-12);
        prev = v;
    }
    std::println("PASS\n");
}

// ── Tween reaches target exactly after its duration ─────────────────────────
void test_tween_basic() {
    std::println("--- test_tween_basic ---");
    auto t = Tween<double>(0.0, 100.0, 1.0, ease::linear);
    assert(approx(t.value(), 0.0));
    assert(!t.done());
    t.tick(0.5);
    assert(approx(t.value(), 50.0));        // linear midpoint
    t.tick(0.5);
    assert(approx(t.value(), 100.0));
    assert(t.done());
    // Over-tick stays pinned at target.
    t.tick(5.0);
    assert(approx(t.value(), 100.0));
    std::println("PASS\n");
}

void test_tween_zero_duration() {
    std::println("--- test_tween_zero_duration ---");
    auto t = Tween<int>(3, 9, 0.0);
    assert(t.value() == 9);   // instantaneous
    assert(t.done());
    std::println("PASS\n");
}

void test_tween_retarget_continuity() {
    std::println("--- test_tween_retarget_continuity ---");
    auto t = Tween<double>(0.0, 100.0, 1.0, ease::linear);
    t.tick(0.3);
    double mid = t.value();                 // ~30
    t.retarget(0.0, 1.0);                   // reverse from current value
    assert(approx(t.value(), mid));         // no jump at retarget
    t.tick(1.0);
    assert(approx(t.value(), 0.0));
    std::println("PASS\n");
}

// ── Spring converges to target and reports rest ─────────────────────────────
void test_spring_converges() {
    std::println("--- test_spring_converges ---");
    auto s = Spring<double>(0.0, spring_presets::snappy);
    s.set_target(1.0);
    // Integrate ~3 seconds at 60fps.
    for (int i = 0; i < 180 && !s.done(); ++i) s.tick(1.0 / 60.0);
    assert(s.done());
    assert(approx(s.value(), 1.0, 1e-2));
    std::println("PASS\n");
}

void test_spring_critical_no_overshoot() {
    std::println("--- test_spring_critical_no_overshoot ---");
    // ζ = 1 (stiff preset) should not overshoot past the target.
    auto s = Spring<double>(0.0, spring_presets::stiff);
    s.set_target(1.0);
    double maxv = 0.0;
    for (int i = 0; i < 240 && !s.done(); ++i) {
        s.tick(1.0 / 60.0);
        maxv = std::max(maxv, s.value());
    }
    assert(maxv <= 1.0 + 1e-3);   // critically damped: no meaningful overshoot
    std::println("PASS\n");
}

void test_spring_underdamped_overshoots() {
    std::println("--- test_spring_underdamped_overshoots ---");
    auto s = Spring<double>(0.0, spring_presets::wobbly);  // ζ = 0.45
    s.set_target(1.0);
    double maxv = 0.0;
    for (int i = 0; i < 240 && !s.done(); ++i) {
        s.tick(1.0 / 60.0);
        maxv = std::max(maxv, s.value());
    }
    assert(maxv > 1.0);   // underdamped overshoots
    assert(s.done());     // ...but still settles
    std::println("PASS\n");
}

void test_spring_dropped_frame_stable() {
    std::println("--- test_spring_dropped_frame_stable ---");
    // A huge dt (a dropped frame / paused tab) must not blow up.
    auto s = Spring<double>(0.0, spring_presets::stiff);
    s.set_target(1.0);
    s.tick(5.0);  // one giant step — clamped + sub-stepped internally
    assert(std::isfinite(s.value()));
    assert(s.value() <= 1.1 && s.value() >= 0.0);
    std::println("PASS\n");
}

void test_spring_zeta_validation() {
    std::println("--- test_spring_zeta_validation ---");
    constexpr SpringParams p = spring_presets::make(200.0, 0.5);
    static_assert(p.stiffness == 200.0);
    assert(approx(p.zeta(), 0.5, 1e-6));
    std::println("PASS\n");
}

// ── Animated<T> wrapper dispatches correctly ────────────────────────────────
void test_animated_tween_mode() {
    std::println("--- test_animated_tween_mode ---");
    auto a = Animated<double>::tween(0.0, 10.0, 1.0, ease::linear);
    assert(!a.is_spring());
    a.tick(0.5);
    assert(approx(a.value(), 5.0));
    a.tick(0.5);
    assert(a.done() && approx(a.value(), 10.0));
    std::println("PASS\n");
}

void test_animated_spring_mode() {
    std::println("--- test_animated_spring_mode ---");
    auto a = Animated<double>::spring(0.0, spring_presets::gentle);
    assert(a.is_spring());
    a.set_target(1.0);
    for (int i = 0; i < 300 && !a.done(); ++i) a.tick(1.0 / 60.0);
    assert(a.done() && approx(a.value(), 1.0, 1e-2));
    std::println("PASS\n");
}

// ── Colour interpolation ────────────────────────────────────────────────────
void test_color_lerp() {
    std::println("--- test_color_lerp ---");
    Color a = Color::rgb(0, 0, 0);
    Color b = Color::rgb(255, 255, 255);
    Color mid = anim::lerp(a, b, 0.5);
    assert(mid.r() == 128 && mid.g() == 128 && mid.b() == 128);
    assert(anim::lerp(a, b, 0.0).r() == 0);
    assert(anim::lerp(a, b, 1.0).r() == 255);

    // Colour tween endpoints.
    auto t = Tween<Color>(Color::rgb(0, 0, 0), Color::rgb(100, 200, 50), 1.0,
                          ease::linear);
    t.tick(1.0);
    Color end = t.value();
    assert(end.r() == 100 && end.g() == 200 && end.b() == 50);
    std::println("PASS\n");
}

void test_color_spring() {
    std::println("--- test_color_spring ---");
    auto s = Spring<Color>(Color::rgb(0, 0, 0), spring_presets::snappy);
    s.set_target(Color::rgb(200, 100, 50));
    for (int i = 0; i < 240 && !s.done(); ++i) s.tick(1.0 / 60.0);
    assert(s.done());
    Color v = s.value();
    assert(std::abs(int(v.r()) - 200) <= 2);
    assert(std::abs(int(v.g()) - 100) <= 2);
    assert(std::abs(int(v.b()) - 50) <= 2);
    std::println("PASS\n");
}

int main() {
    test_easing_constexpr();
    test_tween_basic();
    test_tween_zero_duration();
    test_tween_retarget_continuity();
    test_spring_converges();
    test_spring_critical_no_overshoot();
    test_spring_underdamped_overshoots();
    test_spring_dropped_frame_stable();
    test_spring_zeta_validation();
    test_animated_tween_mode();
    test_animated_spring_mode();
    test_color_lerp();
    test_color_spring();
    std::println("All animation tests passed.");
    return 0;
}
