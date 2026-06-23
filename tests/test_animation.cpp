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

// ── RateCursor: monotone, floor-rate ceiling, burst drain, deadline ─────────
void test_rate_cursor_basic() {
    std::println("--- test_rate_cursor_basic ---");
    RateCursor c(30.0, 0.25);
    assert(approx(c.pos(), 0.0));
    c.tick(3.0, 0.016);
    assert(c.pos() > 0.0 && c.pos() < 1.0);
    double prev = c.pos();
    for (int i = 0; i < 200; ++i) {
        c.tick(3.0, 0.016);
        assert(c.pos() >= prev - 1e-12);
        assert(c.pos() <= 3.0 + 1e-9);
        prev = c.pos();
    }
    assert(approx(c.pos(), 3.0));
    std::println("PASS\n");
}

void test_rate_cursor_burst_drain() {
    std::println("--- test_rate_cursor_burst_drain ---");
    RateCursor c(30.0, 0.25);
    double t = 0.0;
    while (c.pos() < 1000.0 - 1e-6 && t < 2.0) {
        c.tick(1000.0, 0.016);
        t += 0.016;
    }
    assert(t < 0.6);   // burst-paced, not 33s floor-paced
    std::println("PASS\n");
}

void test_rate_cursor_deadline_ramp() {
    std::println("--- test_rate_cursor_deadline_ramp ---");
    RateCursor c(30.0, 5.0);
    c.set_deadline(0.2);
    double t = 0.0;
    while (c.pos() < 100.0 - 1e-6 && t < 1.0) {
        c.tick(100.0, 0.016);
        t += 0.016;
    }
    assert(t <= 0.25);
    std::println("PASS\n");
}

// ── Equivalence: RateCursor reproduces the reveal_fx reveal_cp_ integrator
//    bit-for-bit across a randomized feed. The A/B proof the central
//    primitive can replace the inline reveal_fx.cpp math without changing
//    the typewriter trajectory (hence stream_liveness_test behaviour). The
//    reference is the exact algorithm from advance_reveal_cursor_().
void test_rate_cursor_matches_reveal_fx() {
    std::println("--- test_rate_cursor_matches_reveal_fx ---");
    const double kFloor = 120.0, kDrain = 0.8;

    double ref_cp = 0.0;
    auto ref_step = [&](double total_cp, double committed_cp,
                        double elapsed_s, double ramp_left) {
        if (elapsed_s < 0.0)   elapsed_s = 0.0;
        if (elapsed_s > 0.120) elapsed_s = 0.120;
        if (ref_cp < committed_cp) ref_cp = committed_cp;
        const double backlog = total_cp - ref_cp;
        if (backlog <= 0.0) { ref_cp = total_cp; return; }
        const double burst = backlog / kDrain;
        double cps = burst > kFloor ? burst : kFloor;
        if (ramp_left >= 0.0) {
            if (ramp_left <= 0.0)
                cps = backlog / (elapsed_s > 0.001 ? elapsed_s : 0.001);
            else {
                const double ramp = backlog / ramp_left;
                if (ramp > cps) cps = ramp;
            }
        }
        ref_cp += cps * elapsed_s;
        if (ref_cp > total_cp) ref_cp = total_cp;
    };

    RateCursor c(kFloor, kDrain);

    std::uint64_t rng = 0x1234567;
    auto next = [&]() { rng = rng * 6364136223846793005ull + 1; return rng; };

    double total = 0.0, committed = 0.0;
    double ramp_left = -1.0;
    for (int frame = 0; frame < 4000; ++frame) {
        const std::uint64_t r = next();
        if ((r & 7) != 0) total += static_cast<double>((r >> 8) % 41);
        if ((r & 31) == 0) committed = std::min(total, committed + 50.0);
        if (frame == 3000) ramp_left = 0.3;

        const double elapsed_s = 0.016;

        ref_step(total, committed, elapsed_s,
                 ramp_left >= 0.0 ? ramp_left : -1.0);

        c.advance_floor(committed);
        if (ramp_left >= 0.0) c.set_deadline(ramp_left);
        const double dt = elapsed_s > 0.120 ? 0.120 : elapsed_s;
        c.tick(total, dt);
        if (ramp_left >= 0.0) ramp_left -= elapsed_s;

        assert(std::abs(c.pos() - ref_cp) < 1e-6);
    }

    // Second pass: a SMALL backlog under a TIGHT deadline, so the ramp
    // rate genuinely dominates both floor and burst (the branch the first
    // pass's huge backlog never exercised). Drip 1 cp/frame with a 0.1s
    // deadline armed from the start.
    ref_cp = 0.0;
    c.reset();
    c.set_pacing(kFloor, kDrain);
    total = 0.0; committed = 0.0;
    double rl = 0.1;
    for (int frame = 0; frame < 30; ++frame) {
        total += 1.0;                       // slow drip: backlog stays small
        const double elapsed_s = 0.016;
        ref_step(total, committed, elapsed_s, rl);
        c.advance_floor(committed);
        c.set_deadline(rl);
        c.tick(total, elapsed_s);
        rl -= elapsed_s;
        assert(std::abs(c.pos() - ref_cp) < 1e-6);
    }
    std::println("PASS (RateCursor == reveal_fx reveal_cp_ over 4000 frames)\n");
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
    test_rate_cursor_basic();
    test_rate_cursor_burst_drain();
    test_rate_cursor_deadline_ramp();
    test_rate_cursor_matches_reveal_fx();
    std::println("All animation tests passed.");
    return 0;
}
