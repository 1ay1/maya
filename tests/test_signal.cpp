// Tests for the reactive signal system: Signal, Computed, Effect, Batch
#include <maya/maya.hpp>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <print>

using namespace maya;

TEST_CASE("signal get set") {
    std::println("--- test_signal_get_set ---");
    Signal<int> s{0};
    assert(s.get() == 0);
    s.set(42);
    assert(s.get() == 42);
    std::println("PASS\n");
}

TEST_CASE("signal update") {
    std::println("--- test_signal_update ---");
    Signal<int> s{10};
    s.update([](int& v) { v += 5; });
    assert(s.get() == 15);
    s.update([](int& v) { v *= 2; });
    assert(s.get() == 30);
    std::println("PASS\n");
}

TEST_CASE("signal string") {
    std::println("--- test_signal_string ---");
    Signal<std::string> s{"hello"};
    assert(s.get() == "hello");
    s.set("world");
    assert(s.get() == "world");
    std::println("PASS\n");
}

TEST_CASE("computed basic") {
    std::println("--- test_computed_basic ---");
    Signal<int> x{5};
    auto doubled = computed([&] { return x.get() * 2; });
    assert(doubled.get() == 10);
    x.set(7);
    assert(doubled.get() == 14);
    std::println("PASS\n");
}

TEST_CASE("computed chain") {
    std::println("--- test_computed_chain ---");
    Signal<int> a{3};
    auto b = computed([&] { return a.get() * 2; });     // b = a*2
    auto c = computed([&] { return b.get() + 1; });     // c = b+1 = a*2+1
    auto d = computed([&] { return c.get() * c.get(); }); // d = c^2

    assert(b.get() == 6);
    assert(c.get() == 7);
    assert(d.get() == 49);

    a.set(4);
    assert(b.get() == 8);
    assert(c.get() == 9);
    assert(d.get() == 81);
    std::println("PASS\n");
}

TEST_CASE("computed memoization") {
    std::println("--- test_computed_memoization ---");
    Signal<int> x{1};
    int eval_count = 0;
    auto c = computed([&] { ++eval_count; return x.get() + 1; });

    (void)c.get(); (void)c.get(); (void)c.get(); // multiple reads without signal change
    // Should only have evaluated once since x hasn't changed
    assert(eval_count == 1);

    x.set(2);
    (void)c.get();
    assert(eval_count == 2); // re-evaluated after signal change
    std::println("PASS\n");
}

TEST_CASE("effect fires on construction") {
    std::println("--- test_effect_fires_on_construction ---");
    Signal<int> x{0};
    int count = 0;
    {
        Effect e([&] { (void)x.get(); ++count; });
        assert(count == 1); // fires immediately
    }
    std::println("PASS\n");
}

TEST_CASE("effect fires on signal change") {
    std::println("--- test_effect_fires_on_signal_change ---");
    Signal<int> x{0};
    int last = -1;
    {
        Effect e([&] { last = x.get(); });
        assert(last == 0);
        x.set(10);
        assert(last == 10);
        x.set(20);
        assert(last == 20);
    }
    std::println("PASS\n");
}

TEST_CASE("effect does not fire after destruction") {
    std::println("--- test_effect_does_not_fire_after_destruction ---");
    Signal<int> x{0};
    int count = 0;
    {
        Effect e([&] { (void)x.get(); ++count; });
        assert(count == 1);
    }
    // Effect destroyed — further signal changes should not fire it
    x.set(99);
    assert(count == 1);
    std::println("PASS\n");
}

TEST_CASE("effect multiple dependencies") {
    std::println("--- test_effect_multiple_dependencies ---");
    Signal<int> a{1};
    Signal<int> b{2};
    int sum = 0;
    {
        Effect e([&] { sum = a.get() + b.get(); });
        assert(sum == 3);
        a.set(10);
        assert(sum == 12);
        b.set(20);
        assert(sum == 30);
    }
    std::println("PASS\n");
}

TEST_CASE("batch coalesces updates") {
    std::println("--- test_batch_coalesces_updates ---");
    Signal<int> x{0};
    int fire_count = 0;
    {
        Effect e([&] { (void)x.get(); ++fire_count; });
        assert(fire_count == 1);

        {
            Batch batch;
            x.set(1);
            x.set(2);
            x.set(3);
            // Inside batch: effect must not have re-fired
            assert(fire_count == 1);
        }
        // After batch ends: effect fires exactly once
        assert(fire_count == 2);
        assert(x.get() == 3);
    }
    std::println("PASS\n");
}

TEST_CASE("batch multiple signals") {
    std::println("--- test_batch_multiple_signals ---");
    Signal<int> a{0}, b{0};
    int fire_count = 0;
    {
        Effect e([&] { (void)a.get(); (void)b.get(); ++fire_count; });
        assert(fire_count == 1);
        {
            Batch batch;
            a.set(1);
            b.set(1);
        }
        // One flush, not two
        assert(fire_count == 2);
    }
    std::println("PASS\n");
}

TEST_CASE("multiple effects all notified") {
    std::println("--- test_multiple_effects_all_notified ---");
    Signal<int> x{0};
    int count1 = 0, count2 = 0, count3 = 0;
    {
        Effect e1([&] { (void)x.get(); ++count1; });
        Effect e2([&] { (void)x.get(); ++count2; });
        Effect e3([&] { (void)x.get(); ++count3; });
        assert(count1 == 1 && count2 == 1 && count3 == 1);
        x.set(99);
        assert(count1 == 2 && count2 == 2 && count3 == 2);
    }
    std::println("PASS\n");
}

TEST_CASE("diamond dependency no double fire") {
    std::println("--- test_diamond_dependency_no_double_fire ---");
    // Diamond: a → b, a → c, (b+c) → effect
    // Changing a should fire the effect once, not twice
    Signal<int> a{1};
    auto b = computed([&] { return a.get() * 2; });
    auto c = computed([&] { return a.get() + 1; });
    int fire_count = 0;
    {
        Effect e([&] { (void)b.get(); (void)c.get(); ++fire_count; });
        assert(fire_count == 1);
        a.set(5);
        // With O(1) batch dedup (pending flag), should fire exactly once
        assert(fire_count == 2);
    }
    std::println("PASS\n");
}

TEST_CASE("signal bool") {
    std::println("--- test_signal_bool ---");
    Signal<bool> flag{false};
    assert(!flag.get());
    flag.set(true);
    assert(flag.get());
    flag.update([](bool& v) { v = !v; });
    assert(!flag.get());
    std::println("PASS\n");
}

TEST_CASE("computed with effect") {
    std::println("--- test_computed_with_effect ---");
    Signal<int> x{2};
    auto squared = computed([&] { return x.get() * x.get(); });
    int last_squared = 0;
    {
        Effect e([&] { last_squared = squared.get(); });
        assert(last_squared == 4);
        x.set(5);
        assert(last_squared == 25);
        x.set(10);
        assert(last_squared == 100);
    }
    std::println("PASS\n");
}

// ── Node-lifetime regression tests ────────────────────────────────────
// Each of these was a use-after-free before ~ReactiveNode::unlink_all()
// + the NotifyFrame machinery. Run under ASan to prove the negative.

TEST_CASE("computed destroyed before signal") {
    std::println("--- test_computed_destroyed_before_signal ---");
    // A Computed dropped while its source Signal lives must unsubscribe
    // itself; the next set() then walks a clean subscriber list. Before
    // the fix, Computed::Node had no destructor — the signal kept a raw
    // pointer to the freed node and set() called evaluate() through it.
    Signal<int> x{1};
    {
        auto doubled = computed([&] { return x.get() * 2; });
        assert(doubled.get() == 2);
    }   // doubled's node destroyed here
    x.set(7);            // UAF before the fix; clean notify after
    assert(x.get() == 7);
    std::println("PASS\n");
}

TEST_CASE("signal destroyed before effect") {
    std::println("--- test_signal_destroyed_before_effect ---");
    // A Signal dropped while a dependent Effect lives must remove itself
    // from the effect's dependency list; the effect's later destruction
    // (or re-evaluation) must not call unsubscribe() through the freed
    // signal node. Capture BY REFERENCE deliberately — by-value capture
    // shares the node and hides the bug.
    int runs = 0;
    std::optional<Effect> fx;
    {
        Signal<int> temp{5};
        fx.emplace([&temp, &runs] { (void)temp.get(); ++runs; });
        assert(runs == 1);
    }   // temp's node destroyed; fx still alive with a (now unlinked) dep
    fx.reset();          // UAF before the fix (clear_dependencies → freed node)
    std::println("PASS\n");
}

TEST_CASE("effect disposes sibling effect") {
    std::println("--- test_effect_disposes_sibling_effect ---");
    // Effect A's callback disposes Effect B while both are in the same
    // notification snapshot. B's entry must be nulled (NotifyFrame), not
    // evaluated post-free. Order matters: A subscribes first so it runs
    // first and B is still pending in the snapshot when A kills it.
    Signal<int> s{0};
    auto b = std::make_unique<Effect>();
    int b_runs = 0;
    Effect a([&] {
        (void)s.get();
        if (s.get() > 0) b.reset();   // destroy B mid-notification
    });
    *b = Effect([&] { (void)s.get(); ++b_runs; });
    assert(b_runs == 1);
    s.set(1);            // A runs, kills B; B's snapshot slot nulled
    assert(b_runs == 1); // B must NOT have run after destruction
    s.set(2);            // subsequent sets stay clean
    std::println("PASS\n");
}

TEST_CASE("batched node destroyed before flush") {
    std::println("--- test_batched_node_destroyed_before_flush ---");
    // A node queued in pending_notifications() and destroyed before the
    // batch flushes must be purged from the queue (route 3 of
    // unlink_all). Before the fix flush_batch() evaluated the freed node.
    Signal<int> s{0};
    int runs = 0;
    {
        auto fx = std::make_unique<Effect>([&] { (void)s.get(); ++runs; });
        assert(runs == 1);
        Batch batch;
        s.set(1);        // fx's node queued, deferred
        fx.reset();      // destroyed while queued
    }                    // batch flushes here
    assert(runs == 1);   // never ran post-destruction
    std::println("PASS\n");
}

TEST_CASE("dynamic dependencies switch cleanly") {
    std::println("--- test_dynamic_dependencies_switch_cleanly ---");
    Signal<bool> choose_left{true};
    Signal<int> left{1}, right{10};
    int runs = 0;
    int observed = 0;
    {
        Effect e([&] {
            ++runs;
            observed = choose_left.get() ? left.get() : right.get();
        });
        assert(runs == 1 && observed == 1);
        left.set(2);
        assert(runs == 2 && observed == 2);
        choose_left.set(false);
        assert(runs == 3 && observed == 10);
        left.set(3);  // stale branch must have been unsubscribed
        assert(runs == 3);
        right.set(11);
        assert(runs == 4 && observed == 11);
    }
    std::println("PASS\n");
}

TEST_CASE("dynamic computed dependencies in batch") {
    std::println("--- test_dynamic_computed_dependencies_in_batch ---");
    Signal<bool> choose_left{true};
    Signal<int> left{1}, right{10};
    auto selected = computed([&] { return choose_left.get() ? left.get() : right.get(); });
    int runs = 0;
    Effect e([&] { (void)selected.get(); ++runs; });
    assert(runs == 1);
    {
        Batch batch;
        choose_left.set(false);
        right.set(11);
    }
    assert(selected.get() == 11 && runs == 2);
    left.set(2);  // Computed must no longer depend on the old branch.
    assert(runs == 2);
    std::println("PASS\n");
}

// Regression for the epoch-tracking diamond glitch: when b and c are pulled
// LAZILY from inside the effect body, recomputing c must not synchronously
// re-fire the effect that is still mid-evaluation. This is the exact shape
// that broke when dependency edges became retained (no eager teardown).
TEST_CASE("diamond deep no glitch") {
    std::println("--- test_diamond_deep_no_glitch ---");
    // a -> b -> d,  a -> c -> d,  d -> effect  (asymmetric-depth diamond)
    Signal<int> a{1};
    auto b = computed([&] { return a.get() * 2; });          // depth 1
    auto c = computed([&] { return a.get() + 1; });          // depth 1
    auto d = computed([&] { return b.get() + c.get(); });    // depth 2, joins
    int fire = 0;
    int seen = 0;
    {
        Effect e([&] { seen = d.get(); ++fire; });
        assert(fire == 1 && seen == (1 * 2) + (1 + 1));   // 2 + 2 = 4
        a.set(5);
        // d recomputes once; effect must fire exactly once more.
        assert(fire == 2 && seen == (5 * 2) + (5 + 1));   // 10 + 6 = 16
    }
    std::println("PASS\n");
}

// Same diamond, but the whole mutation is wrapped in a Batch. The pending
// flag dedups the batched path; assert it also produces exactly one fire and
// the fully-consistent value (no torn read of one branch).
TEST_CASE("diamond no glitch in batch") {
    std::println("--- test_diamond_no_glitch_in_batch ---");
    Signal<int> a{1};
    auto b = computed([&] { return a.get() * 2; });
    auto c = computed([&] { return a.get() + 1; });
    int fire = 0;
    int sb = 0, sc = 0;
    {
        Effect e([&] { sb = b.get(); sc = c.get(); ++fire; });
        assert(fire == 1);
        {
            Batch batch;
            a.set(5);
        }
        assert(fire == 2 && sb == 10 && sc == 6);
    }
    std::println("PASS\n");
}

// Wide fan-in: one source feeds N computeds that all feed one effect. The
// effect must fire exactly once per source change regardless of N — the
// per-source O(1) evaluating-guard, not O(N) bookkeeping, keeps this glitch
// free.
TEST_CASE("wide fanin single fire") {
    std::println("--- test_wide_fanin_single_fire ---");
    Signal<int> a{0};
    std::vector<Computed<int>> mids;
    for (int i = 0; i < 8; ++i)
        mids.push_back(computed([&a, i] { return a.get() + i; }));
    int fire = 0;
    {
        Effect e([&] {
            int acc = 0;
            for (auto& m : mids) acc += m.get();
            (void)acc;
            ++fire;
        });
        assert(fire == 1);
        a.set(100);
        assert(fire == 2);  // one coalesced fire, not 8
        a.set(200);
        assert(fire == 3);
    }
    std::println("PASS\n");
}

