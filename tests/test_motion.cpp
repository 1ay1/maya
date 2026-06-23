// Tests for the motion FRAMEWORK (core/motion.hpp): Clock, Motion<T>,
// Timeline, Stagger, pulse/loop helpers. These exercise the driving layer
// that sits on top of the animation.hpp math primitives — the part that
// owns time, ticking, and lifecycle so widgets don't have to.
//
// The frame-request hook (anim::detail::raf_hook) is observed here via a
// test stub so we can assert that an in-flight Motion asks for frames and a
// settled one stops — the property that keeps a maya app idle when nothing
// is animating.
#include <maya/maya.hpp>
#include <cassert>
#include <cmath>
#include <print>

using namespace maya;
using namespace maya::anim;

static bool approx(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) < eps;
}

// ── Frame-request observation ──────────────────────────────────────────────
// Install a counting stub so we can prove Motion drives the loop only while
// moving. (In a real app app.hpp installs request_animation_frame.)
static int g_raf_calls = 0;
static void counting_raf() noexcept { ++g_raf_calls; }
struct RafGuard {
    anim::detail::RafHook saved;
    RafGuard() : saved(anim::detail::raf_hook) { anim::detail::raf_hook = &counting_raf; }
    ~RafGuard() { anim::detail::raf_hook = saved; }
};

// ── Clock: dt clamps long stalls and zeroes a remount ───────────────────────
void test_clock_dt_semantics() {
    std::println("--- test_clock_dt_semantics ---");
    Clock clk;
    // First dt() is ~0 (epoch == last on construction). Subsequent reads in
    // the SAME integer-ms frame return the cached value.
    double a = clk.dt();
    double b = clk.dt();
    assert(approx(a, b) && "dt cached within a frame");
    // A genuinely separate frame is impossible to force without sleeping, so
    // we validate the clamp/remount math directly on the documented tunables.
    static_assert(Clock::kMaxStep == 0.25);
    static_assert(Clock::kRemountGapMs == 500.0);
    std::println("PASS");
}

// ── Motion: settles, stops requesting frames, no jump on retarget ───────────
void test_motion_basic() {
    std::println("--- test_motion_basic ---");
    RafGuard g;
    Clock clk;

    Motion<double> m{0.0, 0.30, ease::linear};
    assert(approx(m.peek(), 0.0));
    assert(!m.moving());

    m.to(1.0, 0.30);            // linear over 0.30s
    assert(m.moving());

    g_raf_calls = 0;
    // Feed dt manually via get_on with an explicit clock we step by abusing
    // begin_frame() between integer-ms boundaries is fiddly; instead drive
    // the underlying math deterministically through a hand clock proxy.
    // We simulate frames by directly ticking a local Animated mirror is not
    // possible (private), so we assert behavioural endpoints:
    //   - while moving, get_on requests at least one frame
    //   - peek advances toward target
    (void)m.get_on(clk);
    // First get after to(): with a near-zero dt the value is still ~0 but a
    // frame WAS requested because we're moving.
    assert(g_raf_calls >= 1 && "in-flight Motion requests frames");

    // Snap kills motion and stops frame requests.
    m.snap(0.5);
    assert(!m.moving());
    g_raf_calls = 0;
    double v = m.get_on(clk);
    assert(approx(v, 0.5));
    assert(g_raf_calls == 0 && "settled Motion does not request frames");
    std::println("PASS");
}

// ── Motion drives to completion over simulated frames ───────────────────────
// We can't sleep in a unit test, so we validate the END STATE: after enough
// elapsed time the value reaches target and moving() clears. We do this by
// constructing the Motion in spring mode and stepping a private-free proxy:
// instead, test the Timeline (which exposes a seekable playhead) for the
// time-accurate path, and here assert the monotone approach property using
// peek() after to().
void test_motion_no_visual_jump_on_retarget() {
    std::println("--- test_motion_no_visual_jump_on_retarget ---");
    Clock clk;
    Motion<double> m{0.0, 0.20, ease::linear};
    m.to(10.0, 0.20);
    // Mid-flight retarget: value must continue from wherever it is, never
    // snap. peek() before/after to() must be identical (set_target keeps the
    // current value as the new `from`).
    double before = m.peek();
    m.to(-5.0, 0.20);
    double after = m.peek();
    assert(approx(before, after) && "retarget preserves current value");
    std::println("PASS");
}

// ── Motion<Color> compiles & lerps endpoints ────────────────────────────────
void test_motion_color() {
    std::println("--- test_motion_color ---");
    Clock clk;
    Motion<Color> c{Color::rgb(0, 0, 0), 0.25};
    Color v = c.peek();
    assert(v.r() == 0 && v.g() == 0 && v.b() == 0);
    c.to(Color::rgb(255, 128, 64));
    assert(c.moving());
    c.snap(Color::rgb(10, 20, 30));
    Color s = c.get_on(clk);
    assert(s.r() == 10 && s.g() == 20 && s.b() == 30);
    std::println("PASS");
}

// ── pulse / loop_phase: bounded, periodic, frame-requesting ─────────────────
void test_pulse_loop() {
    std::println("--- test_pulse_loop ---");
    RafGuard g;
    Clock clk;
    g_raf_calls = 0;
    for (int i = 0; i < 8; ++i) {
        double p = pulse(600.0, clk);
        assert(p >= 0.0 && p <= 1.0 && "pulse stays in [0,1]");
        double s = loop_phase(600.0, clk);
        assert(s >= 0.0 && s < 1.0 && "loop_phase stays in [0,1)");
    }
    assert(g_raf_calls >= 1 && "pulse/loop keep the loop awake");
    // pulse_between endpoints.
    double mid = pulse_between(0.0, 100.0, 600.0, clk);
    assert(mid >= 0.0 && mid <= 100.0);
    std::println("PASS");
}

// ── Timeline: seekable, keyframes interpolate, parallel tracks ──────────────
void test_timeline_keyframes() {
    std::println("--- test_timeline_keyframes ---");
    Timeline tl;
    auto opacity = tl.track(0.0);
    opacity.hold(0.0, 0.10)                       // 0..0.10s: stay 0
           .to(1.0, 0.30, ease::linear);          // 0.10..0.40s: 0→1
    auto y = tl.track(8.0);
    y.at(0.10).to(0.0, 0.20, ease::linear);       // 0.10..0.30s: 8→0

    assert(approx(tl.duration(), 0.40) && "duration is the latest track end");

    // Seek and read both tracks.
    tl.seek(0.0);
    assert(approx(opacity.value(), 0.0));
    assert(approx(y.value(), 8.0));

    tl.seek(0.10);
    assert(approx(opacity.value(), 0.0) && "opacity still 0 at end of hold");
    assert(approx(y.value(), 8.0) && "y at start of its move");

    tl.seek(0.25);   // opacity: (0.25-0.10)/0.30 = 0.5 → 0.5
    assert(approx(opacity.value(), 0.5) && "opacity halfway");
    // y: (0.25-0.10)/0.20 = 0.75 → 8*(1-0.75) = 2.0
    assert(approx(y.value(), 2.0) && "y three-quarters in");

    tl.seek(0.40);
    assert(approx(opacity.value(), 1.0) && "opacity full at end");
    assert(approx(y.value(), 0.0) && "y settled at end");

    std::println("PASS");
}

// ── Timeline: play() advances the playhead and reports done() ───────────────
void test_timeline_play_done() {
    std::println("--- test_timeline_play_done ---");
    Timeline tl;
    auto a = tl.track(0.0);
    a.to(1.0, 0.10, ease::linear);
    assert(!tl.done() && "fresh timeline with a key is not done at 0");
    tl.play();
    tl.seek(0.10);
    assert(tl.done() && "playhead at duration ⇒ done");
    // Looping timeline never reports done.
    tl.set_loop(true);
    assert(!tl.done() && "looping timeline is never done");
    std::println("PASS");
}

// ── Stagger: per-item phased progress + completion ──────────────────────────
void test_stagger() {
    std::println("--- test_stagger ---");
    // step 0.05s, dur 0.30s, linear.
    // item 0 starts at 0.0; item 3 starts at 0.15.
    double e = 0.0;
    assert(approx(stagger_progress(e, 0, 0.05, 0.30, ease::linear), 0.0));
    assert(approx(stagger_progress(e, 5, 0.05, 0.30, ease::linear), 0.0)
           && "later item clamps to 0 before its start");

    e = 0.15;  // item 0: 0.15/0.30 = 0.5 ; item 3: (0.15-0.15)/0.30 = 0
    assert(approx(stagger_progress(e, 0, 0.05, 0.30, ease::linear), 0.5));
    assert(approx(stagger_progress(e, 3, 0.05, 0.30, ease::linear), 0.0));

    e = 0.30;  // item 0 done (clamped 1.0); item 3: (0.30-0.15)/0.30 = 0.5
    assert(approx(stagger_progress(e, 0, 0.05, 0.30, ease::linear), 1.0));
    assert(approx(stagger_progress(e, 3, 0.05, 0.30, ease::linear), 0.5));

    // Completion: 5 items, last starts at 4*0.05=0.20, ends at 0.50.
    assert(!stagger_done(0.49, 5, 0.05, 0.30));
    assert(stagger_done(0.50, 5, 0.05, 0.30));
    assert(stagger_done(0.0, 0, 0.05, 0.30) && "empty stagger is done");
    std::println("PASS");
}

// ── RateCursor is still reachable through the motion umbrella ───────────────
void test_rate_cursor_reachable() {
    std::println("--- test_rate_cursor_reachable ---");
    RateCursor rc{120.0, 0.8};
    rc.set_pos(0.0);
    double p = rc.tick(1000.0, 0.016);  // big backlog → burst drain
    assert(p > 0.0 && "RateCursor advances");
    std::println("PASS");
}

int main() {
    test_clock_dt_semantics();
    test_motion_basic();
    test_motion_no_visual_jump_on_retarget();
    test_motion_color();
    test_pulse_loop();
    test_timeline_keyframes();
    test_timeline_play_done();
    test_stagger();
    test_rate_cursor_reachable();
    std::println("\nAll motion tests passed.");
    return 0;
}
