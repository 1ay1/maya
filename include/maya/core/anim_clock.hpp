#pragma once
// maya::anim_clock — the animation wall clock, with a test-time skew
//
// Every time-driven widget (StreamingMarkdown's reveal cursor / scramble
// window / finalize ramp, ActivityIndicator's tape scroll) derives its
// phase from ONE clock: anim_now_ms(). In production this is exactly
// steady_clock (skew == 0, a single relaxed atomic load on top of the
// clock read — free).
//
// The skew exists for the wall-clock-bound integration tests
// (reveal_scrollback_test, scrollback_oracle_test): they must render
// frames with the animation clock ADVANCING between them — that motion
// is the thing under test (reveal overlay mutating rows near the
// scrollback seam). Before the skew they did
// `sleep_for(20ms)` per frame, which made the suites take minutes of
// pure sleeping (reveal_scrollback_test: ~280 s wall, ~99% asleep).
// With `maya::testing::advance_anim_clock_ms(20)` the same 20 ms of
// ANIMATION time passes with zero wall time: every widget computes the
// exact same ages/phases/deadlines it would have after a real sleep,
// because they all read this one clock.
//
// Monotonicity: tests only ever ADD to the skew, so anim_now_ms() stays
// monotone (steady_clock is monotone, skew is non-decreasing). Widgets
// that latch "first time X happened" timestamps stay consistent.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace maya {

namespace detail {
inline std::atomic<std::int64_t>& anim_clock_skew_ms() noexcept {
    static std::atomic<std::int64_t> skew{0};
    return skew;
}

// Test-only absolute freeze. When >= 0 it REPLACES steady_clock entirely:
// anim_now_ms() returns exactly this value (+ skew), so real wall-clock no
// longer leaks into time-driven widget phases. -1 = disabled (production).
//
// Why a freeze and not just skew: skew is added on top of a live
// steady_clock, so two synchronous build()/render calls a few hundred µs
// apart still see DIFFERENT anim_now_ms under load. Any invariant test that
// drives frames synchronously and asserts frame-to-frame stability (e.g.
// the committed-scrollback-prefix contract) then flakes purely on
// scheduler jitter. Freezing pins time so a synchronous render sequence is
// a pure function of the model; advance_anim_clock_ms still moves it
// forward deterministically when a test WANTS time to pass.
inline std::atomic<std::int64_t>& anim_clock_frozen_ms() noexcept {
    static std::atomic<std::int64_t> frozen{-1};
    return frozen;
}

[[nodiscard]] inline std::int64_t anim_base_ms() noexcept {
    const std::int64_t frozen =
        anim_clock_frozen_ms().load(std::memory_order_relaxed);
    if (frozen >= 0) return frozen;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] inline std::int64_t anim_base_us() noexcept {
    const std::int64_t frozen =
        anim_clock_frozen_ms().load(std::memory_order_relaxed);
    if (frozen >= 0) return frozen * 1000;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace detail

/// Milliseconds on the animation clock: base (steady_clock, or a frozen
/// test value) + test skew. The ONLY clock time-driven widget phases may
/// read.
[[nodiscard]] inline std::int64_t anim_now_ms() noexcept {
    return detail::anim_base_ms()
         + detail::anim_clock_skew_ms().load(std::memory_order_relaxed);
}

/// Microseconds on the animation clock: steady_clock + test skew.
///
/// Companion to anim_now_ms() for the ONE place ms granularity is too
/// coarse: the reveal cursor's per-frame dt. anim_now_ms() truncates to
/// whole milliseconds, so two build() calls landing in the same ms give a
/// dt of 0 — the RateCursor early-outs (dt <= 0) and the cursor doesn't
/// move that frame, bunching motion into the next ms tick (a micro-stutter
/// that is WORSE on fast/local terminals, where sub-ms frames are common).
/// Microsecond resolution removes that quantum. The test skew is still
/// ms-granular (advance_anim_clock_ms) — it is applied here ×1000 so
/// anim_now_us() advances in lock-step with anim_now_ms() under a test
/// skew, staying monotone and phase-consistent with every ms-based widget.
[[nodiscard]] inline std::int64_t anim_now_us() noexcept {
    return detail::anim_base_us()
         + detail::anim_clock_skew_ms().load(std::memory_order_relaxed) * 1000;
}

namespace testing {

/// Advance the animation clock by `ms` WITHOUT sleeping. Test-only:
/// production code must never call this. Additive and monotone —
/// there is deliberately no API to rewind.
inline void advance_anim_clock_ms(std::int64_t ms) noexcept {
    if (ms > 0)
        detail::anim_clock_skew_ms()
            .fetch_add(ms, std::memory_order_relaxed);
}

/// Freeze the animation clock at a fixed base (default 0) so real
/// wall-clock stops leaking in. After this, anim_now_ms() advances ONLY
/// via advance_anim_clock_ms() — making a synchronous render/build
/// sequence a pure function of model state. Test-only.
inline void freeze_anim_clock(std::int64_t at_ms = 0) noexcept {
    detail::anim_clock_frozen_ms().store(at_ms < 0 ? 0 : at_ms,
                                         std::memory_order_relaxed);
}

/// Restore the live steady_clock base (undo freeze_anim_clock). Test-only.
inline void unfreeze_anim_clock() noexcept {
    detail::anim_clock_frozen_ms().store(-1, std::memory_order_relaxed);
}

} // namespace testing
} // namespace maya
