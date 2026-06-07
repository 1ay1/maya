// test_tracked.cpp — pin Tracked<T> behavior.
//
// Tracked<T, Owner, Invalidator> is the wrapper that makes "mutator
// forgets to invalidate cache" structurally impossible. These tests pin
// the contract:
//
//   1. Every write path runs the Invalidator exactly once.
//   2. Reads do NOT run the Invalidator.
//   3. The implicit `const T&` read view works in boolean / arithmetic
//      / comparison contexts (so existing call sites stay unchanged).
//   4. Same-value writes still invalidate — there's no auto-skip-if-equal
//      to second-guess. Cheap, and correct under all use patterns.
//   5. The wrapper is move-safe via rebind(): after the owner is moved,
//      rebind() repoints the back-pointer at the new home.
//
// These tests don't touch StreamingMarkdown — they're a unit-level pin
// on Tracked itself, so any future regression in the wrapper fails here
// FIRST, before higher-level cache tests trip.

#include "maya/core/tracked.hpp"
#include "check.hpp"

#include <chrono>
#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>

using namespace maya;

// ── harness ────────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

template <typename F>
static void run(const char* name, F&& fn) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    try {
        fn();
        auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        std::println("  {:<54} ok    [{} ms]", name, ms);
        ++g_passed;
    } catch (const std::exception& e) {
        ++g_failed;
        std::println("  {:<54} FAIL", name);
        std::println("      {}", e.what());
    }
}

#define REQUIRE(cond, msg) do { \
    if (!(cond)) throw std::runtime_error(std::string("REQUIRE(") + #cond + "): " + (msg)); \
} while (0)

// ── fixture ────────────────────────────────────────────────────────────────

// Minimal Owner: just counts invalidations. Real owners (StreamingMarkdown)
// flip a build_dirty_ flag; here the counter is the more useful probe.
struct Owner {
    int invalidations = 0;

    struct Bump {
        void operator()(Owner& o) const noexcept { ++o.invalidations; }
    };

    Tracked<bool, Owner, Bump>        flag {*this, false};
    Tracked<int,  Owner, Bump>        n    {*this, 0};
    Tracked<std::string, Owner, Bump> name {*this, std::string{"init"}};
};

// ── tests ──────────────────────────────────────────────────────────────────

// T1. Assignment via operator= runs the invalidator exactly once.
static void assignment_invalidates_once() {
    Owner o;
    REQUIRE(o.invalidations == 0, "fresh owner has zero invalidations");
    o.flag = true;
    REQUIRE(o.invalidations == 1, "one write → one invalidation");
    o.flag = false;
    REQUIRE(o.invalidations == 2, "two writes → two invalidations");
}

// T2. .set() is equivalent to operator=.
static void set_method_equivalent_to_assign() {
    Owner o;
    o.flag.set(true);
    REQUIRE(o.invalidations == 1, ".set bumps the invalidator");
    REQUIRE(static_cast<bool>(o.flag) == true, ".set actually stores");
}

// T3. Reads do NOT invalidate.
static void reads_do_not_invalidate() {
    Owner o;
    o.flag = true;
    int base = o.invalidations;

    // Implicit conversion in a boolean context.
    if (o.flag) { (void)0; }
    // Implicit conversion via comparison.
    bool same = (static_cast<bool>(o.flag) == true);
    (void)same;
    // .get() accessor.
    const bool& ref = o.flag.get();
    (void)ref;
    // Arithmetic read via implicit conversion on int Tracked.
    int sum = o.n + 1;
    (void)sum;

    REQUIRE(o.invalidations == base,
            "reads must not bump the invalidator counter");
}

// T4. Same-value writes still invalidate (no auto-skip-if-equal).
// Rationale: Tracked can't know whether the cache that depends on this
// value is keyed only on the value or also on auxiliary state. Always
// invalidating is the safe default; same-value writes are cheap.
static void same_value_writes_still_invalidate() {
    Owner o;
    o.flag = false;             // was false → still false
    REQUIRE(o.invalidations == 1, "same-value write still bumps");
    o.flag = false;
    REQUIRE(o.invalidations == 2, "second same-value write still bumps");
}

// T5. operator++ on arithmetic Tracked bumps once.
static void increment_bumps_once() {
    Owner o;
    o.n++;
    REQUIRE(o.invalidations == 1, "post-increment bumps");
    REQUIRE(static_cast<int>(o.n) == 1, "post-increment stores");
    ++o.n;
    REQUIRE(o.invalidations == 2, "pre-increment bumps");
    REQUIRE(static_cast<int>(o.n) == 2, "pre-increment stores");
}

// T6. Move-construct + rebind keeps the wrapper pointing at the new owner.
// The owner's move-constructor must call rebind() on each Tracked member.
static void move_with_rebind_works() {
    auto make = []() -> Owner {
        Owner o;
        o.flag = true;          // 1 invalidation on the source
        return o;
    };
    Owner dst = make();
    dst.flag.rebind(dst);       // simulate owner move-ctor rebinding
    dst.n.rebind(dst);
    dst.name.rebind(dst);

    int base = dst.invalidations;
    dst.flag = false;           // write through the rebound wrapper
    REQUIRE(dst.invalidations == base + 1,
            "after rebind, writes invalidate the NEW owner");
}

// T7. The implicit const T& view works in switch / comparison / printf.
// Compile-only-ish probe: if these don't compile the wrapper would have
// silently broken every call site.
static void read_view_works_in_common_contexts() {
    Owner o;
    o.flag = true;
    o.n = 42;
    o.name = std::string{"hello"};

    // boolean
    bool b = o.flag;
    REQUIRE(b, "bool conversion");

    // arithmetic
    int v = o.n + 1;
    REQUIRE(v == 43, "int conversion and arithmetic");

    // string member access via .get()
    REQUIRE(o.name.get() == "hello", ".get() returns const ref");

    // ternary
    int chosen = o.flag ? 1 : 0;
    REQUIRE(chosen == 1, "ternary read");
}

// T8. The structural guarantee: there is no path from "write to Tracked"
// to "Owner not invalidated". This is the whole point — encoded as a
// fuzz: every public mutation path must bump.
static void no_mutation_skips_invalidator() {
    Owner o;
    int before = o.invalidations;

    o.flag = true;                          // operator= from T
    o.flag.set(false);                      // explicit .set
    o.n = 5;                                // arithmetic operator=
    o.n.set(7);                             // arithmetic .set
    o.n++;                                  // post-increment
    ++o.n;                                  // pre-increment
    o.name = std::string{"x"};              // string operator=
    o.name.set(std::string{"y"});           // string .set

    // 8 mutations → 8 invalidations, no skips.
    REQUIRE(o.invalidations - before == 8,
            "every public mutation path bumps exactly once");
}

// ── main ───────────────────────────────────────────────────────────────────

int main() {
    std::println("=== test_tracked ===");

    run("assignment invalidates once",           assignment_invalidates_once);
    run("set() equivalent to assign",            set_method_equivalent_to_assign);
    run("reads do not invalidate",               reads_do_not_invalidate);
    run("same-value writes still invalidate",    same_value_writes_still_invalidate);
    run("increment bumps once",                  increment_bumps_once);
    run("move with rebind works",                move_with_rebind_works);
    run("read view works in common contexts",    read_view_works_in_common_contexts);
    run("no mutation skips invalidator",         no_mutation_skips_invalidator);

    std::println("\n── summary ──────────────────────────────────────────────");
    std::println("  passed: {}   failed: {}", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
