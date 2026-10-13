// test_coalesce_never_drops — a frame that was never composed is still owed.
//
// THE BUG. Runtime::render() has two paths that return ok() WITHOUT painting:
// the wire-residue bail-out (the tty rejected bytes with WouldBlock) and the
// adaptive coalesce gate (a congested wire batches frames). Both looked, from
// the caller's side, exactly like a successful paint — same ok() status, no
// bytes either way. The run loop then did `needs_render = false` on ok(), so
// the request was consumed and the frame was not deferred, it was DROPPED.
//
// Streaming hid it: the next delta re-fired within milliseconds and the diff
// is cumulative, so a dropped frame was invisible. A ONE-SHOT model change
// has nothing behind it. Holding an arrow in agentty's theme picker composed
// ~28 KB per keystroke, saturated the writer, and 28 of those previews never
// reached the wire — the screen kept the previous scheme until some unrelated
// keypress happened to repaint. That is the "pressing down doesn't update the
// view, but pressing it twice does" report.
//
// THE FIX. The two states are different claims and must be asked separately:
//   has_pending_writes()  — bytes are queued (residue)
//   has_deferred_frame()  — a compose never happened (coalesce), NO bytes
// A coalesced frame queues nothing, so has_pending_writes() is false for it
// and could never have covered this case. The loop now keeps needs_render set
// while either is true.
//
// This test pins the invariant at the level it actually lives: whatever the
// gates decide, a render request is never silently consumed.

#include <doctest/doctest.h>

#include <maya/device/wire_coalesce.hpp>

#include <cstdio>

namespace {

// The loop's rule, extracted so it can be driven without a terminal:
// a request survives exactly when the runtime still owes a paint.
struct Loop {
    bool needs_render = false;
    bool painted      = false;

    // One iteration. `composed` is what render() did; the gates report
    // whether a frame is still owed afterwards.
    void iterate(bool composed, bool deferred_frame, bool pending_writes) {
        if (!needs_render) return;
        if (composed) painted = true;
        // THE RULE UNDER TEST (mirrors run<P>: needs_render =
        // rt.has_deferred_frame(), plus the pending-writes retry).
        needs_render = deferred_frame || pending_writes;
    }
};

} // namespace

TEST_CASE("coalesce: a frame that was never composed is still owed") {
    // ── 1. A coalesced frame is retried, not dropped ────────────────────
    // The exact shape of the theme-preview bug: one model change, the gate
    // coalesces, and nothing else will ever ask again.
    {
        Loop l;
        l.needs_render = true;                  // a keystroke changed the model

        // Frame 1: the gate coalesces. No compose, and NO bytes queued —
        // which is why has_pending_writes() (false) could not save it.
        l.iterate(/*composed=*/false, /*deferred_frame=*/true,
                  /*pending_writes=*/false);
        CHECK(l.needs_render);   // the request survives
        CHECK(!l.painted);       // and has not painted yet

        // Frame 2: the interval elapsed, so this one composes.
        l.iterate(/*composed=*/true, /*deferred_frame=*/false,
                  /*pending_writes=*/false);
        CHECK(l.painted);
        CHECK(!l.needs_render);  // now consumed
    }

    // ── 2. A wire-blocked frame is retried too ──────────────────────────
    {
        Loop l;
        l.needs_render = true;
        l.iterate(false, /*deferred_frame=*/true, /*pending_writes=*/true);
        CHECK(l.needs_render);
        CHECK(!l.painted);
        l.iterate(true, false, false);
        CHECK(l.painted);
    }

    // ── 3. A congested burst loses NOTHING ──────────────────────────────
    // Hold the key down: many one-shot changes against a wire that
    // coalesces most of them. Every change must eventually reach the
    // screen, at every congestion depth.
    {
        int lost = 0;
        for (int run = 0; run < 12; ++run) {
            Loop l;
            l.needs_render = true;
            for (int i = 0; i < run; ++i)
                l.iterate(false, /*deferred=*/true, false);
            l.iterate(/*composed=*/true, false, false);
            if (!l.painted) ++lost;
        }
        CHECK(lost == 0);
    }

    // ── 4. The hash gate may still consume a request ────────────────────
    // "Nothing changed" and "changed but not painted" are different claims.
    // An ordinary painted frame must NOT keep the loop awake, or an idle
    // app spins forever.
    {
        Loop l;
        l.needs_render = true;
        l.iterate(/*composed=*/true, false, false);
        CHECK(!l.needs_render);
    }
}

TEST_CASE("coalesce: congestion must not defer forever") {
    // A frame owed is only safe if the gate eventually yields. Drive the
    // real CoalesceState with a permanently congested wire and require a
    // compose to get through within a bounded window.
    maya::detail::CoalesceState cs;
    double now = 0.0;
    int composes = 0;
    for (int i = 0; i < 2000; ++i) {          // 2 s at 1 ms steps
        if (!cs.should_coalesce(now, /*congested_now=*/true)) ++composes;
        now += 1.0;
    }
    std::printf("  permanently congested 2000ms -> %d composes\n", composes);
    CHECK(composes > 0);
}
