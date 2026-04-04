# Signals & Reactivity

maya includes a fine-grained reactive signal system inspired by SolidJS and
Leptos. Signals hold mutable values. Computed nodes derive values lazily.
Effects run side-effects. All dependency tracking is automatic.

## Signal\<T\> — Reactive Mutable Value

A `Signal` holds a value and notifies subscribers when it changes:

```cpp
Signal<int> count{0};

count.get();   // Read: returns 0
count();       // Shorthand for get()
count.set(42); // Write: notifies all dependents
count.update([](int& n) { n += 10; }); // Mutate in-place
```

### Construction

```cpp
Signal<int> a{0};                    // int, starts at 0
Signal<std::string> name{"alice"};   // string
Signal<float> ratio{0.5f};          // float
Signal<std::vector<int>> nums{{1, 2, 3}};  // vector
```

### Reading — get() and operator()

```cpp
const int& val = count.get();   // Returns const reference
const int& val = count();       // Same thing
```

**Automatic tracking**: If `get()` is called inside a `Computed` or `Effect`
scope, the signal is automatically registered as a dependency. When the signal
changes, the computed/effect re-evaluates.

### Writing — set()

```cpp
count.set(42);
```

If the new value equals the old value (via `operator==`), no notification is
sent. This prevents unnecessary re-renders.

### Mutating — update()

```cpp
count.update([](int& n) { n *= 2; });
```

`update()` always notifies, since it can't know if the mutation changed the
value. Use `set()` when possible for the equality check.

### Derived Values — map()

```cpp
Signal<int> width{10};
auto doubled = width.map([](int w) { return w * 2; });
// doubled is a Computed<int> that tracks width
```

### Version Tracking

```cpp
uint64_t v = count.version();  // Monotonically increasing on each change
```

## Computed\<T\> — Lazy Derived Value

A `Computed` is a read-only reactive value derived from other signals. It's
memoized — only recomputes when at least one dependency has changed:

```cpp
Signal<int> width{10};
Signal<int> height{20};

auto area = computed([&] {
    return width.get() * height.get();
});

area.get();   // Returns 200
area();       // Same thing

width.set(15);
area.get();   // Returns 300 (recomputed because width changed)
area.get();   // Returns 300 (cached — nothing changed)
```

### How It Works

1. On creation, the compute function runs once to establish initial
   dependencies (via `get()` calls inside the lambda).
2. When any dependency changes, the computed node is marked dirty.
3. On the next `get()`, it re-evaluates, re-establishes dependencies, and
   caches the new value.
4. If the new value equals the old (via `operator==`), downstream subscribers
   are **not** notified — the change stops propagating.

### Chaining

Computed values can depend on other computed values:

```cpp
Signal<float> price{10.0f};
Signal<int> quantity{3};

auto subtotal = computed([&] { return price() * quantity(); });
auto tax      = computed([&] { return subtotal() * 0.08f; });
auto total    = computed([&] { return subtotal() + tax(); });

total.get();  // 32.4
price.set(20.0f);
total.get();  // 64.8 (subtotal, tax, and total all recomputed)
```

## Effect — Reactive Side-Effects

An `Effect` runs a function whenever its dependencies change. It's an RAII
object — alive as long as it exists, disposed when destroyed:

```cpp
Signal<int> count{0};

auto fx = effect([&] {
    std::println("count is now {}", count.get());
});
// Prints: "count is now 0" (runs immediately on creation)

count.set(5);
// Prints: "count is now 5"

count.set(5);
// Nothing printed (value didn't change, no notification)

count.set(10);
// Prints: "count is now 10"
```

### Lifecycle

```cpp
{
    auto fx = effect([&] { /* ... */ });
    // Effect is active
    count.set(1);  // Effect runs
}
// fx destroyed — effect is disposed, no longer runs
count.set(2);  // Effect does NOT run
```

### Manual Disposal

```cpp
auto fx = effect([&] { /* ... */ });
fx.active();   // true
fx.dispose();  // Unsubscribes from all dependencies
fx.active();   // false
```

### Use Cases

- Logging state changes
- Triggering side-effects (network calls, file writes)
- Synchronizing external state with signal values
- Calling `maya::quit()` when a condition is met

```cpp
auto quit_effect = effect([&] {
    if (error_count.get() > 10) {
        maya::quit();
    }
});
```

## Batch — Coalescing Updates

When setting multiple signals, each `set()` triggers notifications immediately.
`Batch` defers all notifications until the batch scope ends:

```cpp
Signal<int> x{0}, y{0};

auto fx = effect([&] {
    std::println("x={}, y={}", x(), y());
});
// Prints: "x=0, y=0"

// Without batch:
x.set(10);  // Effect runs: "x=10, y=0"
y.set(20);  // Effect runs: "x=10, y=20"  (two evaluations!)

// With batch:
{
    Batch batch;
    x.set(10);  // Deferred
    y.set(20);  // Deferred
}
// Effect runs ONCE: "x=10, y=20"
```

### Functional Form

```cpp
batch([&] {
    x.set(10);
    y.set(20);
});
// Single notification after lambda completes
```

### Nested Batches

Batches nest correctly — only the outermost batch triggers notifications:

```cpp
{
    Batch outer;
    x.set(1);
    {
        Batch inner;
        y.set(2);
    }  // inner batch ends, but outer is still active — no notification yet
}  // outer batch ends — notifications fire now
```

## Signals in maya Applications

### Pattern: Signal-Driven State

```cpp
Signal<int>         count{0};
Signal<std::string> message{"Ready"};

run(
    [&](const Event& ev) {
        on(ev, '+', [&] {
            count.update([](int& n) { ++n; });
            message.set("Count: " + std::to_string(count.get()));
        });
        return !key(ev, 'q');
    },
    [&] {
        return (v(
            dyn([&] { return text(message.get()); }),
            dyn([&] { return text(count.get()) | Bold; })
        ) | pad<1>).build();
    }
);
```

### Pattern: Computed Display Values

```cpp
Signal<float> cpu{0.0f};
Signal<float> mem{0.0f};

auto health = computed([&] {
    float c = cpu(), m = mem();
    if (c > 90 || m > 90) return "CRITICAL";
    if (c > 70 || m > 70) return "WARNING";
    return "OK";
});
```

### Pattern: Batch Updates from External Data

```cpp
void on_metrics(const Metrics& m) {
    batch([&] {
        cpu.set(m.cpu);
        mem.set(m.mem);
        disk.set(m.disk);
        net.set(m.net);
    });
    // UI re-renders once with all four values updated
}
```

## Thread-Safety Model

Signals are **thread-local**. Each thread has its own reactive graph with:
- Its own scope stack (`current_scope`)
- Its own batch depth counter
- Its own pending notification queue

Do not share `Signal` objects across threads. For cross-thread communication,
use channels, atomics, or message passing — then update signals on the
receiving thread.

## Reactive Graph Internals

The reactive system uses a simple dependency graph:

```
Signal ──subscribes──→ Computed ──subscribes──→ Effect
  (source)              (derived)               (sink)
```

- **Signals** are pure sources — they have subscribers but no dependencies.
- **Computed** nodes are both — they subscribe to sources and have their own
  subscribers.
- **Effects** are pure sinks — they have dependencies but no subscribers.

When a signal changes:
1. `notify_subscribers()` walks the subscriber list.
2. Each subscriber is `mark_dirty()` + `evaluate()`.
3. `Computed::evaluate()` re-runs the compute function, re-establishes
   dependencies via `track()`, and propagates if the value changed.
4. `Effect::evaluate()` re-runs the effect function.

This is a **push-then-pull** model: changes push "dirty" flags down the graph,
then evaluation pulls fresh values as needed.
