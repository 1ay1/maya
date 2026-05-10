// test_render_scaling.cpp — verify the content-keyed component cache
// (ComponentElement::cache_id) keeps per-frame render time flat as the
// number of "settled turns" grows. Models the host-side pattern: a
// freshly-constructed wrapper Element per frame whose address differs
// from frame to frame, but whose cache_id is stable. Without cache_id
// the renderer's pointer-keyed cache misses on every wrapper, the
// inner component re-runs render() every frame, and per-frame cost
// scales linearly with turn count. With cache_id the content cache
// matches across frames, render() runs once per turn for the lifetime
// of that turn, and per-frame cost stays O(1)-ish per turn (one
// hashmap lookup + one canvas-region reuse).

#include <maya/maya.hpp>
#include <maya/render/renderer.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace maya;
using namespace maya::dsl;

namespace {

struct Counter {
    std::atomic<int> n{0};
};

// One "settled turn"-shaped Element: a vstack with a header text, two
// body lines, and a nested ComponentElement that represents a tool
// card. The nested ComponentElement bumps `counter` whenever its
// render lambda fires — the test reads this counter to distinguish
// cache hits from misses.
Element settled_turn_body(std::shared_ptr<Counter> counter, int turn_idx) {
    Element tool_card = component([counter](int /*w*/, int /*h*/) -> Element {
        counter->n.fetch_add(1, std::memory_order_relaxed);
        Element line1 = text("tool: read");
        Element line2 = text("    /path/to/file.cpp");
        Element line3 = text("    output: ...");
        Element line4 = text("    [exit 0]");
        return v(std::vector<Element>{line1, line2, line3, line4}).build();
    });
    Element header = text("Turn " + std::to_string(turn_idx) + " — header");
    Element body1  = text("body line one — some content here");
    Element body2  = text("body line two — some content here");
    return (v(std::vector<Element>{header, body1, body2, tool_card})
                | padding(0, 1)).build();
}

// Wrap a stable inner Element in a fresh ComponentElement each call.
// When `id` is non-empty, the renderer's content cache matches across
// frames despite the wrapper's address differing. With `id` empty,
// pointer keying takes over — and because we construct a fresh wrapper
// every frame (mirroring `Conversation::build()` push_back-ing into a
// fresh `rows` vector), the pointer never matches the cache and every
// frame is a full miss.
Element make_wrapper(std::shared_ptr<Element> sp, std::string id) {
    Element e = component([sp](int /*w*/, int /*h*/) -> Element {
        return *sp;
    });
    std::get<ComponentElement>(e.inner).cache_id = std::move(id);
    return e;
}

// Build a fresh per-frame vstack of N wrappers. Each call produces
// brand-new wrapper addresses; only cache_id (when set) carries the
// caller's promise that the content is stable.
Element build_frame(const std::vector<std::shared_ptr<Element>>& bodies,
                    bool use_cache_id) {
    std::vector<Element> rows;
    rows.reserve(bodies.size());
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        std::string id = use_cache_id ? "turn:" + std::to_string(i) : std::string{};
        rows.push_back(make_wrapper(bodies[i], std::move(id)));
    }
    return v(rows).build();
}

double render_once_us(const Element& root, int width = 80) {
    StylePool pool;
    Canvas canvas(width, 5000, &pool);
    auto t0 = std::chrono::steady_clock::now();
    render_tree(root, canvas, pool, theme::dark, /*auto_height=*/true);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

// Inner-component render counts after a sequence of frames must equal:
//   with cache_id  → exactly N (one render per turn for its lifetime)
//   without        → N × frame_count (full miss every frame)
// This is the structural assertion — true regardless of machine speed.
void test_cache_id_hits_across_frames() {
    constexpr int kN = 50;
    constexpr int kFrames = 5;
    auto counter = std::make_shared<Counter>();

    std::vector<std::shared_ptr<Element>> bodies;
    bodies.reserve(kN);
    for (int i = 0; i < kN; ++i)
        bodies.push_back(std::make_shared<Element>(settled_turn_body(counter, i)));

    for (int f = 0; f < kFrames; ++f) {
        Element root = build_frame(bodies, /*use_cache_id=*/true);
        (void)render_once_us(root);
    }

    int total = counter->n.load();
    std::printf("[cache_id]  inner renders after %d frames @ N=%d → %d (expect %d)\n",
                kFrames, kN, total, kN);
    assert(total == kN);
}

void test_no_cache_id_misses_every_frame() {
    constexpr int kN = 50;
    constexpr int kFrames = 5;
    auto counter = std::make_shared<Counter>();

    std::vector<std::shared_ptr<Element>> bodies;
    bodies.reserve(kN);
    for (int i = 0; i < kN; ++i)
        bodies.push_back(std::make_shared<Element>(settled_turn_body(counter, i)));

    for (int f = 0; f < kFrames; ++f) {
        Element root = build_frame(bodies, /*use_cache_id=*/false);
        (void)render_once_us(root);
    }

    int total = counter->n.load();
    std::printf("[no-id]     inner renders after %d frames @ N=%d → %d (expect %d)\n",
                kFrames, kN, total, kN * kFrames);
    assert(total == kN * kFrames);
}

// Wall-clock comparison at growing N. The cache_id path's warm-frame
// cost should stay tiny relative to N; the no-id path's should scale
// roughly linearly. We assert a conservative ratio threshold so the
// test passes on slow CI machines while still catching regressions.
void test_warm_frame_time_at_high_turn_count() {
    constexpr int kFrames = 20;
    const std::vector<int> kSizes = {25, 50, 100, 200};

    std::printf("\n== warm-frame render time vs turn count ==\n");
    std::printf("  %5s | %10s | %10s | %6s\n",
                "N", "cache_id us", "no-id us", "ratio");
    std::printf("  ------+------------+------------+-------\n");

    double last_ratio = 0.0;
    for (int N : kSizes) {
        auto counter = std::make_shared<Counter>();
        std::vector<std::shared_ptr<Element>> bodies;
        bodies.reserve(N);
        for (int i = 0; i < N; ++i)
            bodies.push_back(std::make_shared<Element>(settled_turn_body(counter, i)));

        // Cold pass to populate caches under each variant — measured
        // separately so the warm steady-state isn't distorted.
        (void)render_once_us(build_frame(bodies, /*use_cache_id=*/true));
        (void)render_once_us(build_frame(bodies, /*use_cache_id=*/false));

        // Warm passes — average over kFrames.
        double sum_id = 0;
        for (int f = 0; f < kFrames; ++f)
            sum_id += render_once_us(build_frame(bodies, true));
        double avg_id = sum_id / kFrames;

        double sum_no = 0;
        for (int f = 0; f < kFrames; ++f)
            sum_no += render_once_us(build_frame(bodies, false));
        double avg_no = sum_no / kFrames;

        double ratio = avg_no / std::max(1.0, avg_id);
        std::printf("  %5d | %10.0f | %10.0f | %5.1fx\n",
                    N, avg_id, avg_no, ratio);
        last_ratio = ratio;
    }

    // At the largest N, the no-cache_id path should be substantially
    // slower per warm frame than the cache_id path. 3x is a deliberately
    // loose floor — locally this is typically 30-100x, but slow QEMU /
    // CI machines can compress the gap. A regression that lets the
    // pointer-keyed path silently match (e.g., cache_id stops being
    // honored, OR the eviction loop accidentally drops the id map) will
    // collapse the ratio to ~1x and trip this assertion.
    assert(last_ratio > 3.0);
}

} // namespace

int main() {
    test_cache_id_hits_across_frames();
    test_no_cache_id_misses_every_frame();
    test_warm_frame_time_at_high_turn_count();
    std::printf("\ntest_render_scaling: ok\n");
    return 0;
}
