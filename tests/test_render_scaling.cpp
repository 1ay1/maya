// test_render_scaling.cpp — rigorous characterisation of the
// content-keyed component cache (ComponentElement::cache_id) under
// realistic agent-app conditions.
//
// What "proper" means here:
//   - Structural assertions (counter equality) where behaviour must be
//     exact regardless of timing — a cache_id miss is a bug, not a
//     slow path.
//   - Timing budgets where behaviour is a perf claim — these have
//     deliberately loose floors so the test stays robust on slow CI /
//     QEMU machines while still tripping on real regressions.
//   - Long-running variants that simulate the actual reported failure
//     mode ("starts fast, gets slow with time"): hundreds of frames,
//     turn appends with virtualization, cache size monitoring,
//     allocation-pattern stress.
//
// Each test prints a one-line summary so a human reading CI output
// can see the actual numbers, not just "ok / fail."

#include <maya/maya.hpp>
#include <maya/render/renderer.hpp>
#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/tool_body_preview.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// Always-live check, regardless of NDEBUG (the test target ships with
// -O3 -DNDEBUG and a plain `assert(...)` would compile out, silently
// turning a perf regression into a test-pass). CHECK prints the
// failed expression, the file:line, and the (optional) extra context
// the call site supplies, then aborts so ctest reports a failure.
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "\nCHECK failed: %s\n  at %s:%d\n",        \
                         #cond, __FILE__, __LINE__);                        \
            std::fprintf(stderr, "  " __VA_ARGS__);                         \
            std::fprintf(stderr, "\n");                                     \
            std::abort();                                                   \
        }                                                                   \
    } while (0)

namespace {

// ── Shared helpers ───────────────────────────────────────────────────────────

struct Counter {
    std::atomic<int> n{0};
};

// Synthetic "settled turn" — header text + body lines + nested
// ComponentElement (tool card). Mimics the shape agentty produces:
// a vstack with mixed text and component leaves; the inner
// ComponentElement counts its render invocations so tests can
// distinguish cache hits from misses.
Element settled_turn(std::shared_ptr<Counter> counter, int turn_idx,
                     int tool_count = 1) {
    auto make_tool = [&]() {
        Element e = component([counter](int /*w*/, int /*h*/) -> Element {
            counter->n.fetch_add(1, std::memory_order_relaxed);
            std::vector<Element> rows;
            rows.push_back(text("tool: read"));
            rows.push_back(text("    /path/to/file.cpp"));
            rows.push_back(text("    output: ..."));
            rows.push_back(text("    [exit 0]"));
            return v(rows).build();
        });
        return e;
    };
    std::vector<Element> rows;
    rows.push_back(text("Turn " + std::to_string(turn_idx) + " — header"));
    rows.push_back(text("body line one — some content here"));
    rows.push_back(text("body line two — some content here"));
    for (int i = 0; i < tool_count; ++i) rows.push_back(make_tool());
    return (v(rows) | padding(0, 1)).build();
}

// Wrap a stable inner Element in a fresh ComponentElement each call.
// `use_shared_ctor=true` → take the public path that maya consumers
// hit when handing a `shared_ptr<Element>` to any Element slot
// (implicit constructor; the renderer keys its cross-frame cache on
// the shared_ptr's address). `false` → wrap manually via component()
// with no cache identity, exercising the pointer-keyed fallback —
// the wrapper's address is fresh every frame, so this path full-
// misses every frame and quantifies what the implicit-ctor path
// saves us from.
Element wrap(std::shared_ptr<Element> sp, bool use_shared_ctor) {
    if (use_shared_ctor) {
        // Implicit Element(shared_ptr<const Element>) conversion —
        // the entire public surface for "render this stable thing
        // efficiently across frames". No id string, no method chain.
        return Element{std::shared_ptr<const Element>{std::move(sp)}};
    }
    return component([sp = std::move(sp)](int /*w*/, int /*h*/) -> Element {
        return *sp;
    });
}

Element build_frame(const std::vector<std::shared_ptr<Element>>& bodies,
                    bool use_shared_ctor) {
    std::vector<Element> rows;
    rows.reserve(bodies.size());
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        rows.push_back(wrap(bodies[i], use_shared_ctor));
    }
    return v(rows).build();
}

double render_us(const Element& root, Canvas& canvas, StylePool& pool) {
    auto t0 = std::chrono::steady_clock::now();
    render_tree(root, canvas, pool, theme::dark, /*auto_height=*/true);
    return std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
}

// Visible size of the renderer's content-keyed cache. Public test
// access through a generic accessor would be cleaner, but reaching
// in via the renderer's own inspection paths keeps the test free of
// internal-header coupling: we render once and read the side-effect
// (component_cache is updated) through observable behaviour.
//
// For now we infer "did the cache stay bounded" from total inner
// renders (counter) — if the cache is leaking, the same instance ID
// will get re-rendered as it's evicted and re-inserted. Direct cache
// size readout would need a new public hook; flagged as a follow-up.

// ── Test 1: cache_id is honoured every frame after the first ────────────────

void test_cache_id_hits_across_frames() {
    constexpr int N = 50;
    constexpr int kFrames = 10;
    auto counter = std::make_shared<Counter>();

    std::vector<std::shared_ptr<Element>> bodies;
    bodies.reserve(N);
    for (int i = 0; i < N; ++i)
        bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

    StylePool pool;
    Canvas canvas(80, 5000, &pool);

    for (int f = 0; f < kFrames; ++f) {
        Element root = build_frame(bodies, /*use_shared_ctor=*/true);
        (void)render_us(root, canvas, pool);
    }

    int total = counter->n.load();
    std::printf("[shared-ctor]   N=%d frames=%d → inner renders = %d (expect %d)\n",
                N, kFrames, total, N);
    CHECK(total == N, "  implicit shared_ptr ctor did not engage cross-frame cache: "
          "%d renders vs %d expected\n", total, N);
}

void test_no_cache_id_misses_every_frame() {
    constexpr int N = 50;
    constexpr int kFrames = 10;
    auto counter = std::make_shared<Counter>();

    std::vector<std::shared_ptr<Element>> bodies;
    bodies.reserve(N);
    for (int i = 0; i < N; ++i)
        bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

    StylePool pool;
    Canvas canvas(80, 5000, &pool);

    for (int f = 0; f < kFrames; ++f) {
        Element root = build_frame(bodies, /*use_shared_ctor=*/false);
        (void)render_us(root, canvas, pool);
    }

    int total = counter->n.load();
    std::printf("[plain-component] N=%d frames=%d → inner renders = %d (expect %d)\n",
                N, kFrames, total, N * kFrames);
    CHECK(total == N * kFrames,
          "  plain component() path should miss every frame: %d vs %d expected\n",
          total, N * kFrames);
}

// ── Test 2: warm-frame time at increasing N ─────────────────────────────────

void test_warm_frame_time_at_high_turn_count() {
    constexpr int kFrames = 30;
    const std::vector<int> kSizes = {25, 50, 100, 200};

    std::printf("\n== warm-frame render time vs turn count ==\n");
    std::printf("  %5s | %10s | %10s | %6s | %12s\n",
                "N", "shared us", "plain us", "ratio", "us per turn");
    std::printf("  ------+------------+------------+--------+--------------\n");

    double last_ratio = 0.0;
    for (int N : kSizes) {
        auto counter = std::make_shared<Counter>();
        std::vector<std::shared_ptr<Element>> bodies;
        bodies.reserve(N);
        for (int i = 0; i < N; ++i)
            bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

        StylePool pool;
        Canvas canvas(80, 5000, &pool);

        // Cold pass under each variant — measured separately so the
        // warm steady-state isn't distorted by first-time miss cost.
        (void)render_us(build_frame(bodies, /*use_shared_ctor=*/true), canvas, pool);
        (void)render_us(build_frame(bodies, /*use_shared_ctor=*/false), canvas, pool);

        double sum_id = 0;
        for (int f = 0; f < kFrames; ++f)
            sum_id += render_us(build_frame(bodies, /*use_shared_ctor=*/true), canvas, pool);
        double avg_id = sum_id / kFrames;

        double sum_no = 0;
        for (int f = 0; f < kFrames; ++f)
            sum_no += render_us(build_frame(bodies, /*use_shared_ctor=*/false), canvas, pool);
        double avg_no = sum_no / kFrames;

        double ratio = avg_no / std::max(1.0, avg_id);
        double per_turn = avg_id / N;
        std::printf("  %5d | %10.0f | %10.0f | %5.1fx | %10.2f\n",
                    N, avg_id, avg_no, ratio, per_turn);
        last_ratio = ratio;
    }

    // At the largest N the shared-ptr path must be substantially
    // faster than the plain component() path. 3x is a deliberately
    // loose floor (locally we see 4-8x); a regression that breaks
    // the content cache or its cells layer collapses the ratio to
    // ~1.0 and trips this CHECK.
    CHECK(last_ratio > 3.0,
          "  shared-ptr speedup vs plain component() collapsed to %.2fx\n",
          last_ratio);
}

// ── Test 3: long session — turn appends with virtualization ─────────────────
//
// Simulates the actual reported failure pattern: a session that
// "starts fast, gets slow with time." The agentty-side discipline is
// a sliding window of `kViewWindow` settled turns; this test models
// that by appending a fresh turn each "frame" and dropping the
// oldest from the visible window once we exceed the cap.
//
// What we assert:
//   1. Per-frame time stays bounded as the SESSION grows past the
//      window cap. After reaching steady-state, frame N+100 should
//      cost no more than ~2x frame N. Without virtualization (or with
//      a broken cache) this would scale linearly with total session
//      length.
//   2. Inner-component renders grow by exactly 1 per appended turn —
//      each new turn is rendered once for its lifetime (in the live
//      window) and then evicted by the LRU/age path when it scrolls
//      out of view. No per-frame re-renders for stable settled turns.

void test_long_session_with_virtualization() {
    constexpr int kViewWindow = 20;
    constexpr int kTotalTurns = 500;
    constexpr int kSampleFrames = 40;       // measure first/last 40
    constexpr int kFramesPerTurn = 1;       // one frame per appended turn

    auto counter = std::make_shared<Counter>();
    std::vector<std::shared_ptr<Element>> all_bodies;
    all_bodies.reserve(kTotalTurns);
    for (int i = 0; i < kTotalTurns; ++i)
        all_bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

    StylePool pool;
    Canvas canvas(80, 5000, &pool);

    std::vector<double> per_frame_us;
    per_frame_us.reserve(kTotalTurns);

    for (int turn = 0; turn < kTotalTurns; ++turn) {
        // Visible window: last `kViewWindow` turns, like agentty's
        // thread_view_start sliding forward.
        int start = std::max(0, (turn + 1) - kViewWindow);
        std::vector<std::shared_ptr<Element>> window(
            all_bodies.begin() + start,
            all_bodies.begin() + turn + 1);

        // Each turn's shared_ptr address IS its cross-frame identity
        // — same as agentty's per-message cache feeding maya.
        std::vector<Element> rows;
        rows.reserve(window.size());
        for (std::size_t i = 0; i < window.size(); ++i) {
            rows.push_back(wrap(window[i], /*use_shared_ctor=*/true));
        }
        Element root = v(rows).build();

        for (int f = 0; f < kFramesPerTurn; ++f) {
            per_frame_us.push_back(render_us(root, canvas, pool));
        }
    }

    // Average over the first sample (cold-ish, first window fills) vs
    // the last sample (steady state, hundreds of turns into the
    // session). If virtualization + cache work, these are similar.
    double early_sum = 0;
    for (int i = kViewWindow; i < kViewWindow + kSampleFrames; ++i)
        early_sum += per_frame_us[i];
    double early_avg = early_sum / kSampleFrames;

    double late_sum = 0;
    for (int i = kTotalTurns - kSampleFrames; i < kTotalTurns; ++i)
        late_sum += per_frame_us[i];
    double late_avg = late_sum / kSampleFrames;

    int total_renders = counter->n.load();
    std::printf("[long-session]  total_turns=%d window=%d → "
                "early avg %.0f us, late avg %.0f us, ratio %.2fx\n",
                kTotalTurns, kViewWindow, early_avg, late_avg, late_avg / early_avg);
    std::printf("                inner renders = %d (expect %d, one per turn lifetime)\n",
                total_renders, kTotalTurns);

    // The session must NOT slow down with time. 2.5x is a generous
    // floor — measurement noise on a 100us baseline can swing 30%
    // either way, but a true regression where settled turns start
    // re-rendering or the cache leaks will blow well past this.
    CHECK(late_avg < early_avg * 2.5,
          "  long-session per-frame time grew %.2fx (early %.0f us, late %.0f us)\n",
          late_avg / early_avg, early_avg, late_avg);

    // Each appended turn renders its inner ComponentElement exactly
    // once: on the frame it first lands in the window. After that
    // it's a cache hit until it scrolls out, and after eviction it's
    // gone. Total = kTotalTurns.
    CHECK(total_renders == kTotalTurns,
          "  unexpected re-renders: %d vs %d expected\n",
          total_renders, kTotalTurns);
}

// ── Test 4: width-change invalidation ───────────────────────────────────────
//
// Cache entries record the width they were rendered at. A width
// change invalidates them. We assert: exactly N misses on the first
// frame at the new width, then steady cache hits.

void test_width_change_invalidates_then_restabilises() {
    constexpr int N = 30;
    auto counter = std::make_shared<Counter>();

    std::vector<std::shared_ptr<Element>> bodies;
    bodies.reserve(N);
    for (int i = 0; i < N; ++i)
        bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

    StylePool pool;
    Canvas canvas(80, 5000, &pool);

    auto frame = [&]() { return build_frame(bodies, /*use_shared_ctor=*/true); };

    // Warm up at width 80.
    Element f1 = frame();
    (void)render_us(f1, canvas, pool);
    int after_warmup = counter->n.load();
    CHECK(after_warmup == N,
          "  initial render: %d vs %d expected\n", after_warmup, N);

    Element f2 = frame();
    (void)render_us(f2, canvas, pool);
    int after_steady = counter->n.load();
    CHECK(after_steady == N,
          "  steady state regressed at same width: %d vs %d expected\n",
          after_steady, N);

    // Resize canvas — width-keyed entries should miss next frame.
    canvas.resize(120, 5000);
    Element f3 = frame();
    (void)render_us(f3, canvas, pool);
    int after_resize = counter->n.load();
    std::printf("[width-change]  resize 80→120: extra renders = %d (expect %d)\n",
                after_resize - after_steady, N);
    CHECK(after_resize - after_steady == N,
          "  width-change should re-render every entry once: %d vs %d expected\n",
          after_resize - after_steady, N);

    // Steady state at the new width.
    Element f4 = frame();
    (void)render_us(f4, canvas, pool);
    int after_resize_steady = counter->n.load();
    CHECK(after_resize_steady == after_resize,
          "  cache failed to re-stabilise after resize: %d vs %d\n",
          after_resize_steady, after_resize);
}

// ── Test 5: live tail dominates a long settled prefix ───────────────────────
//
// The actual agentty workload: K settled turns + 1 live tail (the
// streaming message). Live tail rebuilds every frame; settled turns
// hit cache. Per-frame cost should be dominated by the live tail —
// adding more settled turns should barely move the needle.

void test_live_tail_dominates_settled_prefix() {
    constexpr int kFrames = 30;
    auto counter = std::make_shared<Counter>();

    StylePool pool;
    Canvas canvas(80, 5000, &pool);

    auto measure_with_settled = [&](int K) {
        std::vector<std::shared_ptr<Element>> bodies;
        bodies.reserve(K);
        for (int i = 0; i < K; ++i)
            bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

        // Build a frame: K cached settled wrappers + 1 fresh live tail
        // (no shared_ptr, fully rebuilt every frame — represents the
        // active streaming turn in a live agent session).
        auto build = [&]() {
            std::vector<Element> rows;
            rows.reserve(K + 1);
            for (int i = 0; i < K; ++i)
                rows.push_back(wrap(bodies[i], /*use_shared_ctor=*/true));
            rows.push_back(settled_turn(counter, /*idx=*/9999));
            return v(rows).build();
        };

        // Warm-up.
        (void)render_us(build(), canvas, pool);

        double sum = 0;
        for (int f = 0; f < kFrames; ++f)
            sum += render_us(build(), canvas, pool);
        return sum / kFrames;
    };

    double t_0_settled  = measure_with_settled(0);
    double t_5_settled  = measure_with_settled(5);
    double t_50_settled = measure_with_settled(50);

    std::printf("[live-tail]     0 settled + 1 live: %.1f us\n", t_0_settled);
    std::printf("                5 settled + 1 live: %.1f us\n", t_5_settled);
    std::printf("                50 settled + 1 live: %.1f us\n", t_50_settled);

    // Per-turn marginal cost — what each cached settled turn adds to
    // the per-frame budget. With the canvas-region cache (every
    // cache_id entry stores its painted cells and the fast path
    // blits row-by-row) this is now a memcpy per row of the cached
    // region, not a recursive build_layout_tree + layout::compute +
    // paint_element walk. On the synthetic turn body it measures
    // around 0.2 us; a 1.5 us budget catches the regression where
    // either the cells cache stops populating or the paint phase
    // falls back to recursive layout/paint over the cached Element.
    double per_turn_marginal_us = (t_50_settled - t_0_settled) / 50.0;
    std::printf("                marginal per-turn cost: %.2f us\n",
                per_turn_marginal_us);
    CHECK(per_turn_marginal_us < 1.5,
          "  per-turn cached cost regressed to %.2f us (budget 1.5 us); "
          "the canvas-region cache likely fell back to recursive paint.\n",
          per_turn_marginal_us);
}

// ── Test 6: ephemeral components don't leak the cache ───────────────────────
//
// Components without cache_id are pointer-keyed. Each frame
// constructs fresh wrappers → fresh addresses → cache miss. The
// LRU eviction on each top-level render_tree call drops entries
// whose last_frame predates the previous one, so the pointer-keyed
// map stays bounded over a long session of fresh-each-frame
// components.
//
// We verify: after K=200 frames of fresh wrappers, the inner-component
// counter equals N*K (every frame is a full miss), and the process
// hasn't run out of memory or hit some unbounded-growth pathology.

void test_ephemeral_components_do_not_leak() {
    constexpr int N = 40;
    constexpr int kFrames = 200;
    auto counter = std::make_shared<Counter>();

    std::vector<std::shared_ptr<Element>> bodies;
    bodies.reserve(N);
    for (int i = 0; i < N; ++i)
        bodies.push_back(std::make_shared<Element>(settled_turn(counter, i)));

    StylePool pool;
    Canvas canvas(80, 5000, &pool);

    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        Element root = build_frame(bodies, /*use_shared_ctor=*/false);
        (void)render_us(root, canvas, pool);
    }
    auto wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    int total = counter->n.load();
    std::printf("[no-leak]       N=%d × %d frames (plain component) → "
                "%d inner renders, %.0f ms wall, %.1f us/frame\n",
                N, kFrames, total, wall_ms, wall_ms * 1000.0 / kFrames);
    // Every frame full miss → counter exactly N*kFrames. Off by any
    // amount means the cache is keeping stale entries that should
    // have evicted.
    CHECK(total == N * kFrames,
          "  ephemeral path off-count: %d vs %d expected (cache leak?)\n",
          total, N * kFrames);
    // Wall-clock loose floor: 200 frames × N=40 should complete in
    // well under 30 seconds even on slow QEMU. Catches a regression
    // where cache lookup becomes superlinear.
    CHECK(wall_ms < 30000,
          "  ephemeral-loop wall-time blew the 30s budget: %.0f ms\n", wall_ms);
}

// ── id_for_shared churn ────────────────────────────────────────────────────
//
// Stresses the thread-local weak_ptr→id map inside element.hpp:
// id_for_shared. The renderer's content-keyed cache uses the
// returned id as cache_id; correctness rules:
//
//   1. A shared_ptr handed twice must return the SAME id (so the
//      cross-frame cache hits).
//   2. After the original shared_ptr dies, a fresh shared_ptr at the
//      same allocator-recycled raw address must return a DIFFERENT
//      id (otherwise stale cells from the dead one are blitted under
//      the new one — the ghost-composer bug the comments call out).
//   3. Over many churn cycles the implementation must keep id
//      identity correct AND not let the internal map grow without
//      bound (the bulk-sweep + incremental-sweep paths handle this;
//      this test exercises both by churning > kIncSweepEvery iters).
//
// We can't observe id_for_shared directly (it's in `detail`), but we
// can observe its CONSEQUENCE: the renderer's render-call count. If
// the cache_id derived from id_for_shared is wrong, either we miss
// when we should hit (count too high) or we hit when we should miss
// (count too low — stale cells served under fresh content).

void test_id_for_shared_churn_recycles_ids() {
    constexpr int kChurnCycles = 200;   // > kIncSweepEvery (64) → exercises sweep
    auto counter = std::make_shared<Counter>();

    StylePool pool;
    Canvas canvas(80, 80, &pool);

    int prior_renders = 0;
    int cycles_with_fresh_render = 0;

    // Each cycle: allocate a brand-new shared_ptr, hand it to the
    // implicit Element(shared_ptr) ctor twice, render. The first
    // wrap should miss (fresh id → no cells yet), the second should
    // hit. Then drop the shared_ptr so the next cycle's allocation
    // may recycle its control-block address.
    for (int i = 0; i < kChurnCycles; ++i) {
        auto sp = std::make_shared<Element>(settled_turn(counter, i));

        // Frame N: first appearance → must render() once.
        {
            Element root = (v(
                Element{std::shared_ptr<const Element>{sp}}
            ) | grow(1.0f)).build();
            (void)render_us(root, canvas, pool);
        }
        // Frame N+1: same shared_ptr → same id → cache hit, no render.
        {
            Element root = (v(
                Element{std::shared_ptr<const Element>{sp}}
            ) | grow(1.0f)).build();
            (void)render_us(root, canvas, pool);
        }

        int now = counter->n.load();
        int delta = now - prior_renders;
        // Expectation: exactly +1 render per cycle (the first frame's
        // miss). The second frame in the same cycle must hit cache C.
        // Anything else means id_for_shared aliased ids across cycles
        // (delta=0: stale-cell blit, correctness bug) or failed to
        // cache within a cycle (delta=2: cache_id stability broken).
        if (delta == 1) ++cycles_with_fresh_render;
        prior_renders = now;
        // sp goes out of scope here → control block freed → next
        // iteration's make_shared may land at the same address.
    }

    std::printf("[churn]         %d/%d cycles had exactly 1 fresh render "
                "(total renders = %d, expected = %d)\n",
                cycles_with_fresh_render, kChurnCycles,
                counter->n.load(), kChurnCycles);
    CHECK(cycles_with_fresh_render == kChurnCycles,
          "  id_for_shared churn corrupted cache identity: "
          "%d/%d cycles off-count\n",
          kChurnCycles - cycles_with_fresh_render, kChurnCycles);
    CHECK(counter->n.load() == kChurnCycles,
          "  total renders %d != cycles %d (id aliasing or cache mismatch)\n",
          counter->n.load(), kChurnCycles);
}

// ── AgentTimeline per-event cache_id ───────────────────────────────────
//
// During an agentic loop where ONE tool is still running and several
// have already settled, agentty's turn-level cache (gated on
// is_turn_resolved — every tool terminal, no pending permission)
// can't engage — the whole turn rebuilds every frame for the live
// spinner. Per-event cache_ids on terminal events let those done
// cards' cells be blitted across frames even while the turn as a
// whole is in flight.
//
// We measure the difference: a timeline with N done events + 1 running,
// rendered 200 frames. With cache_id set on the done events, per-frame
// cost should NOT scale linearly with N — each done card's cell blit
// is bounded, and only the running card pays full layout+paint.

void test_agent_timeline_per_event_cache_id_bounds_cost() {
    constexpr int kFrames = 200;
    StylePool pool;
    Canvas canvas(120, 5000, &pool);

    auto make_done = [](int idx, bool with_cache_id) {
        AgentTimelineEvent ev;
        ev.name = "read";
        ev.detail = "src/file.cpp";
        ev.elapsed_seconds = 0.42f;
        ev.category_color = Color::cyan();
        ev.status = AgentEventStatus::Done;
        ev.body.kind = ToolBodyPreview::Kind::FileRead;
        std::string text;
        for (int j = 0; j < 20; ++j)
            text += "line " + std::to_string(j) + " content of the file\n";
        ev.body.text = std::move(text);
        if (with_cache_id) ev.cache_id = "tool:done-" + std::to_string(idx);
        return ev;
    };
    auto make_running = []() {
        AgentTimelineEvent ev;
        ev.name = "bash";
        ev.detail = "running";
        ev.category_color = Color::green();
        ev.status = AgentEventStatus::Running;
        ev.body.kind = ToolBodyPreview::Kind::BashOutput;
        ev.body.text = "intermediate...\n";
        return ev;
    };

    auto measure = [&](int n_done, bool with_cache_id) -> double {
        AgentTimeline::Config base_cfg;
        base_cfg.title = "Actions";
        for (int i = 0; i < n_done; ++i)
            base_cfg.events.push_back(make_done(i, with_cache_id));
        base_cfg.events.push_back(make_running());

        auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f) {
            AgentTimeline::Config cfg = base_cfg;
            cfg.frame = f;
            AgentTimeline tl{cfg};
            Element root = tl.build();
            std::vector<layout::LayoutNode> layout_nodes;
            canvas.clear();
            render_tree(root, canvas, pool, theme::dark, layout_nodes, true);
        }
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(t1 - t0).count() / kFrames;
    };

    double base_no    = measure(0, false);   // 1 running, baseline
    double n10_no     = measure(10, false);  // 10 done + 1 running, NO cache_id
    double n10_cached = measure(10, true);   // 10 done + 1 running, WITH cache_id

    std::printf("[agent-timeline]  baseline (1 running):     %6.1f us/frame\n",
                base_no);
    std::printf("                  10 done + 1 (no cache):  %6.1f us/frame\n",
                n10_no);
    std::printf("                  10 done + 1 (cached):    %6.1f us/frame  (%.2fx speedup)\n",
                n10_cached, n10_no / n10_cached);

    // Structural invariant: with per-event cache_id the cost of 10
    // done cards should be MEANINGFULLY less than without. A 1.2x
    // floor is loose enough to survive CI / QEMU noise while still
    // tripping if the cache wrapper silently stops engaging (e.g.
    // someone moves the cache_id field, drops the ComponentElement
    // wrap, or breaks the renderer's content-keyed lookup).
    CHECK(n10_cached < n10_no,
          "  per-event cache_id did not reduce cost: %.1f us cached >= %.1f us baseline\n",
          n10_cached, n10_no);
    CHECK(n10_no / n10_cached >= 1.2,
          "  per-event cache_id speedup too small: %.2fx (expected >= 1.2x)\n",
          n10_no / n10_cached);
}

} // namespace

int main() {
    test_cache_id_hits_across_frames();
    test_no_cache_id_misses_every_frame();
    test_warm_frame_time_at_high_turn_count();
    test_long_session_with_virtualization();
    test_width_change_invalidates_then_restabilises();
    test_live_tail_dominates_settled_prefix();
    test_ephemeral_components_do_not_leak();
    test_id_for_shared_churn_recycles_ids();
    test_agent_timeline_per_event_cache_id_bounds_cost();
    std::printf("\ntest_render_scaling: ok\n");
    return 0;
}
