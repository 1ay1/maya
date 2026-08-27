#pragma once
// Adaptive wire coalescing — pure decision math.
//
// The orchestration (wall-clock timing, the `return ok()` skip) lives in
// Runtime::render()'s inline path; the *math* lives here so it is unit
// testable in isolation (see tests/test_wire_coalesce.cpp) without spinning
// up a Runtime or a real tty.
//
// Motivation: on a remote link (mosh / SSH / Tailscale) a streaming turn
// firing ~60 RAF/sec pushes ~60 tiny diff frames/sec, each paying a fixed
// CUP+SGR navigation tax — a 12.8x wire amplification the wire_bytes_bench
// measured. Coalescing appends into one cumulative diff frame removes that
// overhead. We coalesce ONLY when the wire is congested, so a fast/local
// wire is entirely unaffected.

#include <algorithm>

namespace maya::detail {

// EWMA update for the congestion estimate. `prev` is the running estimate
// in [0,1]; `congested_now` is whether THIS frame found residue on the wire
// (the wire couldn't take the previous frame in full). Returns the new
// estimate. alpha = 0.25 → ~4-frame memory: reactive enough to ramp up on a
// stall, damped enough to ignore a single-frame hiccup.
[[nodiscard]] constexpr double update_congestion(double prev,
                                                 bool congested_now) noexcept {
    constexpr double alpha = 0.25;
    const double sample = congested_now ? 1.0 : 0.0;
    return (1.0 - alpha) * prev + alpha * sample;
}

// Coalesce interval, in MILLISECONDS, for a given congestion estimate.
//
//   ewma <= floor (0.15)  → 0 ms  (inert: fast/local wire, no coalescing)
//   ewma == 1.0           → cap   (33 ms ≈ 30 fps, fully saturated)
//   in between            → linear ramp
//
// The floor keeps a wire with the occasional hiccup at zero interval (no
// latency added when it isn't needed); the cap bounds the worst-case added
// latency to one 30 fps frame, so a coalesced frame is never delayed beyond
// what a human perceives as smooth, and the caller's RAF re-fire (≤16 ms
// during a stream) always re-attempts it promptly.
[[nodiscard]] constexpr double coalesce_interval_ms(double ewma) noexcept {
    constexpr double kFloor  = 0.15;
    constexpr double kCapMs  = 33.0;
    if (ewma <= kFloor) return 0.0;
    const double frac = (ewma - kFloor) / (1.0 - kFloor);   // (0, 1]
    return std::clamp(frac, 0.0, 1.0) * kCapMs;
}

// Stateful coalesce decision, extracted from Runtime::render() so the exact
// orchestration is unit-testable without a Runtime or a clock. Mirrors the
// render-path logic 1:1: update the EWMA from this frame's residue state,
// then decide whether to COMPOSE (false) or COALESCE/skip (true) based on
// whether the interval has elapsed since the last compose.
struct CoalesceState {
    double congestion = 0.0;
    double last_compose_ms = -1.0;   // < 0 = never composed yet

    // Returns true if this frame should be COALESCED (skipped). `now_ms` is a
    // monotonic timestamp in milliseconds; `congested_now` is whether the
    // wire had residue on entry. On a compose (return false) the last-compose
    // timestamp advances; on a coalesce it does not.
    [[nodiscard]] bool should_coalesce(double now_ms, bool congested_now) noexcept {
        congestion = update_congestion(congestion, congested_now);
        const double interval = coalesce_interval_ms(congestion);
        if (interval <= 0.0) {          // fast wire: always compose
            last_compose_ms = now_ms;
            return false;
        }
        if (last_compose_ms >= 0.0 && (now_ms - last_compose_ms) < interval)
            return true;                // inside the interval: coalesce
        last_compose_ms = now_ms;       // interval elapsed: compose
        return false;
    }
};

} // namespace maya::detail
