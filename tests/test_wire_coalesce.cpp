// test_wire_coalesce — the adaptive wire-coalescing decision math.
//
// Proves the pure functions that Runtime::render() uses to decide whether to
// coalesce a streaming frame under wire backpressure. The integration (the
// `return ok()` skip, wall-clock timing, RAF re-fire) is covered by the
// Witness Chain tests, which must still pass; here we pin the MATH:
//
//   1. On a fast wire (no residue) the interval is 0 → inert, zero added
//      latency, never coalesces. This is the "must not regress local" gate.
//   2. Sustained congestion ramps the interval toward the 33 ms cap.
//   3. The EWMA damps a single-frame hiccup (doesn't over-react).
//   4. The interval is monotonic in congestion and bounded by the cap.

#undef NDEBUG
#include "agtest.hpp"

#include <maya/app/wire_coalesce.hpp>

#include <cstdio>

using namespace maya::detail;

TEST_CASE("wire coalesce: inert on a fast wire") {
    // A wire that never shows residue keeps congestion at 0 → interval 0 →
    // the gate never coalesces. This is the load-bearing "no local
    // regression" property: on a fast/local tty coalescing is a no-op.
    double ewma = 0.0;
    for (int i = 0; i < 100; ++i)
        ewma = update_congestion(ewma, /*congested_now=*/false);
    CHECK(ewma == 0.0, "  fast wire must keep congestion at 0 (got %.6f)\n", ewma);
    CHECK(coalesce_interval_ms(ewma) == 0.0,
          "  fast wire must yield 0 ms interval (got %.3f)\n",
          coalesce_interval_ms(ewma));
}

TEST_CASE("wire coalesce: a lone hiccup does not trip coalescing") {
    // One congested frame amid a fast stream: the EWMA must stay BELOW the
    // 0.15 floor so a single stall doesn't add latency to an otherwise fast
    // wire. alpha=0.25 → one sample lifts ewma to 0.25... which is ABOVE the
    // floor for exactly one frame, then decays. Assert it decays back under
    // the floor within a couple of clean frames (bounded transient).
    double ewma = 0.0;
    ewma = update_congestion(ewma, true);          // the hiccup
    const double after_hiccup = ewma;
    ewma = update_congestion(ewma, false);
    ewma = update_congestion(ewma, false);         // two clean frames
    CHECK(after_hiccup > 0.0, "  a hiccup must register (got %.4f)\n", after_hiccup);
    CHECK(ewma < 0.15,
          "  congestion must decay below the floor after 2 clean frames "
          "(got %.4f)\n", ewma);
    CHECK(coalesce_interval_ms(ewma) == 0.0,
          "  post-hiccup interval must be 0 (got %.3f)\n",
          coalesce_interval_ms(ewma));
}

TEST_CASE("wire coalesce: sustained congestion ramps to the cap") {
    // A wire that stalls every frame drives congestion → 1.0 and the
    // interval → the 33 ms cap (≈30 fps), the maximum batching.
    double ewma = 0.0;
    for (int i = 0; i < 50; ++i)
        ewma = update_congestion(ewma, /*congested_now=*/true);
    CHECK(ewma > 0.99, "  sustained stall must saturate congestion (got %.4f)\n", ewma);
    const double ms = coalesce_interval_ms(ewma);
    CHECK(ms > 30.0 && ms <= 33.0,
          "  saturated interval must approach the 33 ms cap (got %.3f)\n", ms);
}

TEST_CASE("wire coalesce: interval is monotonic and bounded") {
    // The interval must never decrease as congestion rises, and never
    // exceed the cap — so the added latency is predictable and bounded.
    double prev = -1.0;
    for (double e = 0.0; e <= 1.0 + 1e-9; e += 0.05) {
        const double ms = coalesce_interval_ms(e);
        CHECK(ms >= prev - 1e-9,
              "  interval must be monotonic in congestion (e=%.2f ms=%.3f "
              "prev=%.3f)\n", e, ms, prev);
        CHECK(ms <= 33.0 + 1e-9,
              "  interval must never exceed the 33 ms cap (e=%.2f ms=%.3f)\n",
              e, ms);
        prev = ms;
    }
    // Below the floor is strictly zero.
    CHECK(coalesce_interval_ms(0.10) == 0.0,
          "  below-floor congestion must be 0 ms (got %.3f)\n",
          coalesce_interval_ms(0.10));
}

TEST_CASE("wire coalesce: fast wire composes every frame") {
    // Simulate the render loop firing at 60 fps (16.6 ms/frame) on a wire
    // that never congests: EVERY frame must compose (zero coalescing), so a
    // local tty sees no added latency and no dropped frames.
    CoalesceState cs;
    int composed = 0, coalesced = 0;
    for (int i = 0; i < 120; ++i) {
        const double now_ms = i * 16.6;
        if (cs.should_coalesce(now_ms, /*congested_now=*/false)) ++coalesced;
        else ++composed;
    }
    CHECK(coalesced == 0,
          "  fast wire must never coalesce (coalesced=%d)\n", coalesced);
    CHECK(composed == 120,
          "  fast wire must compose every frame (composed=%d/120)\n", composed);
}

TEST_CASE("wire coalesce: congested wire batches frames") {
    // Simulate 60 fps RAF re-fires (16.6 ms apart) on a SATURATED wire
    // (residue every frame). Once congestion ramps past the floor the
    // interval approaches 33 ms, so roughly every OTHER 16.6 ms frame is
    // coalesced — composes drop to ~half, which is the wire saving. The
    // exact ratio depends on the ramp; assert a strict, robust reduction.
    CoalesceState cs;
    int composed = 0, coalesced = 0;
    for (int i = 0; i < 120; ++i) {
        const double now_ms = i * 16.6;
        if (cs.should_coalesce(now_ms, /*congested_now=*/true)) ++coalesced;
        else ++composed;
    }
    std::printf("[wire coalesce]  saturated 60fps: composed=%d coalesced=%d "
                "(%.0f%% frames coalesced)\n",
                composed, coalesced, 100.0 * coalesced / 120.0);
    CHECK(coalesced > 0,
          "  a saturated wire must coalesce some frames (coalesced=%d)\n",
          coalesced);
    // At 16.6 ms cadence vs a ~33 ms interval, at least a third of frames
    // should batch away — the per-frame overhead the bench measured.
    CHECK(composed < 90,
          "  saturated wire must cut composes below 90/120 (got %d)\n",
          composed);
    // And it must never STARVE: every coalesced frame is re-attempted by the
    // next RAF, so composes can't hit zero — the stream still advances.
    CHECK(composed > 0,
          "  coalescing must not starve the stream (composed=%d)\n", composed);
}
