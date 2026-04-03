// Tests for the reactive signal system: Signal, Computed, Effect, Batch
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

void test_signal_get_set() {
    std::println("--- test_signal_get_set ---");
    Signal<int> s{0};
    assert(s.get() == 0);
    s.set(42);
    assert(s.get() == 42);
    std::println("PASS\n");
}

void test_signal_update() {
    std::println("--- test_signal_update ---");
    Signal<int> s{10};
    s.update([](int& v) { v += 5; });
    assert(s.get() == 15);
    s.update([](int& v) { v *= 2; });
    assert(s.get() == 30);
    std::println("PASS\n");
}

void test_signal_string() {
    std::println("--- test_signal_string ---");
    Signal<std::string> s{"hello"};
    assert(s.get() == "hello");
    s.set("world");
    assert(s.get() == "world");
    std::println("PASS\n");
}

void test_computed_basic() {
    std::println("--- test_computed_basic ---");
    Signal<int> x{5};
    auto doubled = computed([&] { return x.get() * 2; });
    assert(doubled.get() == 10);
    x.set(7);
    assert(doubled.get() == 14);
    std::println("PASS\n");
}

void test_computed_chain() {
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

void test_computed_memoization() {
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

void test_effect_fires_on_construction() {
    std::println("--- test_effect_fires_on_construction ---");
    Signal<int> x{0};
    int count = 0;
    {
        Effect e([&] { (void)x.get(); ++count; });
        assert(count == 1); // fires immediately
    }
    std::println("PASS\n");
}

void test_effect_fires_on_signal_change() {
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

void test_effect_does_not_fire_after_destruction() {
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

void test_effect_multiple_dependencies() {
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

void test_batch_coalesces_updates() {
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

void test_batch_multiple_signals() {
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

void test_multiple_effects_all_notified() {
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

void test_diamond_dependency_no_double_fire() {
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

void test_signal_bool() {
    std::println("--- test_signal_bool ---");
    Signal<bool> flag{false};
    assert(!flag.get());
    flag.set(true);
    assert(flag.get());
    flag.update([](bool& v) { v = !v; });
    assert(!flag.get());
    std::println("PASS\n");
}

void test_computed_with_effect() {
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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_signal_get_set();
    test_signal_update();
    test_signal_string();
    test_computed_basic();
    test_computed_chain();
    test_computed_memoization();
    test_effect_fires_on_construction();
    test_effect_fires_on_signal_change();
    test_effect_does_not_fire_after_destruction();
    test_effect_multiple_dependencies();
    test_batch_coalesces_updates();
    test_batch_multiple_signals();
    test_multiple_effects_all_notified();
    test_diamond_dependency_no_double_fire();
    test_signal_bool();
    test_computed_with_effect();
    std::println("=== ALL 16 TESTS PASSED ===");
}
