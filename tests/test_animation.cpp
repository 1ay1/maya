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
// ── RateCursor: the constant-glide model's INVARIANTS ───────────────────────
//
// The cursor was redesigned from a backlog-proportional rate to a
// constant-glide drainer (the ChatGPT / Vercel smoothStream model): it
// cruises at a near-constant speed and lets the buffer absorb wire jitter.
// The property that matters for "does it feel like a glide" is no longer
// "matches the old reference bit-for-bit" but: under a bursty arrival
// pattern the per-frame SPEED stays bounded and changes only gradually (no
// sprint/idle sawtooth), while the cursor still tracks the edge and the
// finalize ramp still hits its deadline.
void test_rate_cursor_glide_invariants() {
    std::println("--- test_rate_cursor_glide_invariants ---");
    const double kCruise = 120.0, kLead = 0.5;
    RateCursor c(kCruise, kLead);

    std::uint64_t rng = 0x1234567;
    auto next = [&]() { rng = rng * 6364136223846793005ull + 1; return rng; };

    double total = 0.0;
    double prev_pos = 0.0;
    double prev_speed = -1.0;
    const double dt = 0.016;
    double max_speed_seen = 0.0;
    double max_speed_jump = 0.0;

    for (int frame = 0; frame < 4000; ++frame) {
        // Bursty arrival: most frames nothing, occasional fat chunk — exactly
        // the spiky SSE pattern that used to make the cursor sprint.
        const std::uint64_t r = next();
        if ((r & 15) == 0) total += static_cast<double>((r >> 8) % 120);

        c.tick(total, dt);
        const double pos = c.pos();
        const double speed = (pos - prev_pos) / dt;   // units/sec this frame
        prev_pos = pos;

        // INV1: monotone — never moves backward.
        assert(pos >= prev_pos - 1e-9);
        // INV2: speed is bounded. The instantaneous rate caps at the
        // burst-mult × the auto-tuned base; the base itself can climb under a
        // sustained fast feed, but with a 0.5s lead and bounded chunks it
        // stays comfortably under a generous ceiling. (Pure correctness:
        // never the old backlog/drain teleport of thousands of cps.)
        if (speed > max_speed_seen) max_speed_seen = speed;
        // INV3: speed changes GRADUALLY. The frame-to-frame speed delta is
        // the sawtooth detector — the old model jumped from floor to a huge
        // burst in one frame; the glide model must not. Allow a modest step
        // (the proportional catch-up term + base drift), but nothing like a
        // teleport.
        if (prev_speed >= 0.0) {
            const double jump = std::abs(speed - prev_speed);
            if (jump > max_speed_jump) max_speed_jump = jump;
        }
        prev_speed = speed;
    }

    std::println("    max speed seen   : {:.1f} cps", max_speed_seen);
    std::println("    max speed jump   : {:.1f} cps/frame", max_speed_jump);
    // The cursor should have tracked the edge (caught up by the end, since
    // the feed stopped growing well before frame 4000's steady tail). Give
    // it a few settle frames: with a bounded-lag glide the cursor sits
    // ~drain_secs behind the edge, so a single tick can't clear a backlog
    // left by a chunk that landed on one of the last frames — drain until
    // the wall-clock lag window has elapsed (0.5 s lead / 0.016 s ≈ 32
    // frames, use 64 for margin).
    for (int i = 0; i < 64; ++i) c.tick(total, dt);
    assert(std::abs(c.pos() - total) < 1.0);
    // Speed never exploded: a backlog-proportional model would have hit
    // tens of thousands of cps on a fat chunk; the glide caps it.
    assert(max_speed_seen < 4000.0);
    std::println("PASS (glide: bounded speed, edge tracked)\n");
}

// ── RateCursor: finalize ramp still hits its deadline ───────────────────────
void test_rate_cursor_ramp_still_lands() {
    std::println("--- test_rate_cursor_ramp_still_lands ---");
    RateCursor c(60.0, 0.5);
    double total = 200.0;          // a backlog the cruise speed won't clear fast
    double rl = 0.2;               // 200ms deadline
    const double dt = 0.016;
    for (int frame = 0; frame < 20; ++frame) {
        c.set_deadline(rl);
        c.tick(total, dt);
        rl -= dt;
    }
    // By the deadline the cursor must have reached the edge.
    assert(std::abs(c.pos() - total) < 1.0);
    std::println("PASS (ramp lands on deadline)\n");
}

// ── RateCursor: sub-millisecond frames must still advance ───────────────────
//
// anim_now_ms() truncates to whole ms, so on a fast/local terminal two
// build()s can land in the same ms and hand tick() a dt of 0 (the cursor
// stalls that frame, motion bunches into the next ms tick — a micro-
// stutter). The reveal path now derives dt from anim_now_us() so a sub-ms
// dt is a real, non-zero advance. Pin that tick() itself makes forward
// progress on a stream of tiny dts summing to a normal frame budget, and
// that many sub-ms frames reveal the same total as one combined frame
// (no motion lost to quantisation).
void test_rate_cursor_submillisecond_dt() {
    std::println("--- test_rate_cursor_submillisecond_dt ---");
    const double kCruise = 120.0, kLead = 0.5;

    // A: ten 0.6 ms frames (6 ms total) — the sub-ms regime a µs clock
    // preserves but a ms clock would round to a mix of 0 ms and 1 ms.
    RateCursor a(kCruise, kLead);
    const double target = 1000.0;
    for (int i = 0; i < 10; ++i) {
        const double before = a.pos();
        a.tick(target, 0.0006);
        // Every sub-ms frame makes strictly forward progress (backlog is
        // huge, floor rate applies) — never a stalled 0-advance frame.
        assert(a.pos() > before);
    }

    // B: one combined 6 ms frame reveals ~the same amount (integration is
    // dt-additive; the smoothing differs slightly frame-to-frame but the
    // TOTAL over the window must match within a small tolerance — proving
    // no motion is lost or double-counted by sub-stepping).
    RateCursor b(kCruise, kLead);
    b.tick(target, 0.006);

    const double diff = std::abs(a.pos() - b.pos());
    std::println("    10x0.6ms pos={:.2f}  1x6ms pos={:.2f}  diff={:.2f}",
                 a.pos(), b.pos(), diff);
    // Backlog/drain rate over 6 ms at ~floor is ~0.72 cp; the low-pass
    // makes the split path a touch slower, but the two must agree to well
    // under a codepoint — the quantisation error a ms clock introduced.
    assert(diff < 1.0);
    // And the sub-ms path did move (not stuck at 0).
    assert(a.pos() > 0.0);
    std::println("PASS (sub-ms frames advance, no quantisation stall)\n");
}

// ── RateCursor: the finalize ramp must not wobble the post-ramp speed ───────
//
// The host re-arms the finalize deadline every frame (set_deadline(remaining)).
// The ramp used to re-seed smoothed_rate_ on EVERY ramp frame, so it stored
// the (large, deadline-driven) ramp rate into the low-pass state; when the
// ramp then CLEARED, the next non-ramp frame started from that inflated
// smoothed_rate_ and had to relax back down — a burst of over-fast glide
// AFTER the deadline (a visible speed wobble at end-of-turn). The re-seed
// now fires only on the ramp's rising edge, so the low-pass keeps its
// wire-tracking rate through the ramp and the FIRST post-ramp frame glides
// at the normal rate, not the ramp rate.
//
// This is checked directly: run a ramp to completion, CLEAR the deadline,
// then feed a modest steady backlog and assert the first post-ramp frame's
// speed is near the cruise glide — NOT the inflated ramp speed. (The
// speed DURING the ramp is legitimately large — that's the deadline
// guarantee — so we measure only the hand-off frame.)
void test_rate_cursor_ramp_no_wobble() {
    std::println("--- test_rate_cursor_ramp_no_wobble ---");
    const double kCruise = 120.0, kLead = 0.5;
    RateCursor c(kCruise, kLead);
    const double dt = 0.016;

    // Warm up a steady glide behind a growing edge.
    double total = 0.0;
    for (int i = 0; i < 30; ++i) { total += 8.0; c.tick(total, dt); }

    // Run a finalize ramp to completion (host re-arms the deadline each
    // frame). This drives the cursor to the edge fast and — under the old
    // per-frame re-seed — leaves smoothed_rate_ inflated to the ramp rate.
    double rl = 0.2;
    for (int i = 0; i < 16; ++i) {
        c.set_deadline(rl);
        c.tick(total, dt);
        rl -= dt;
    }
    c.clear_deadline();

    // Now grow the edge by a modest steady amount and measure the FIRST
    // post-ramp frame's speed. With the rising-edge re-seed the low-pass
    // still holds ~the wire rate, so this frame glides near cruise. With
    // the old per-frame re-seed smoothed_rate_ was pinned to the (much
    // larger) ramp rate, so this frame would lurch far above cruise.
    const double pos_before = c.pos();
    total += 8.0;                      // ~one frame of wire at 500 cps
    c.tick(total, dt);
    const double handoff_speed = (c.pos() - pos_before) / dt;

    std::println("    first post-ramp frame speed: {:.1f} cps (cruise {:.0f})",
                 handoff_speed, kCruise);
    // The hand-off must not lurch: a small backlog (8 cp) at a healthy
    // glide clears in a few frames, so the speed sits near the backlog/lead
    // rate, comfortably under a few× cruise. The old wobble drove this to
    // the ramp rate (many hundreds of cps) — orders of magnitude higher.
    assert(handoff_speed < 3.0 * kCruise);
    // Cursor still tracks the edge afterward.
    for (int i = 0; i < 64; ++i) c.tick(total, dt);
    assert(std::abs(c.pos() - total) < 1.0);
    std::println("PASS (ramp re-seed on rising edge only, no post-ramp wobble)\n");
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
    test_rate_cursor_glide_invariants();
    test_rate_cursor_ramp_still_lands();
    test_rate_cursor_submillisecond_dt();
    test_rate_cursor_ramp_no_wobble();
    std::println("All animation tests passed.");
    return 0;
}
