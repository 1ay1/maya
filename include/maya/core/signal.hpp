#pragma once
// maya::core::signal - Fine-grained reactive signal system
//
// Inspired by SolidJS and Leptos. Replaces React's useState/useEffect with
// a pull-based, lazily-evaluated dependency graph. Signals hold values.
// Computed nodes derive values. Effects run side-effects. All dependency
// tracking is automatic - just call get() inside a reactive scope.
//
// Thread-safety model: signals are thread-local. One reactive graph per
// thread. Cross-thread communication uses channels, not shared signals.

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

// C++23 added std::move_only_function (P0288). It avoids the copy
// overhead of std::function and accepts non-copyable callables — the
// right abstraction for reactive callbacks, which are captured once
// and invoked in place.
//
// libstdc++ 14+ and libc++ 19+ ship it. Older C++23 standard libraries
// (Termux/Android NDK clang 21 still on libc++ 18, GCC 13) don't.
// Feature-test it and fall back to std::function on those — copies of
// the callable are unobservable here (we always invoke through the
// node, never replicate it), and capturing non-copyable types into
// reactive callbacks is rare enough that surfacing the requirement
// as a compile-time error is acceptable.
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
#  define MAYA_DETAIL_CALLABLE ::std::move_only_function
#else
#  define MAYA_DETAIL_CALLABLE ::std::function
#endif

namespace maya {

// ============================================================================
// Forward declarations
// ============================================================================

namespace detail {
    struct ReactiveNode;
    struct ReactiveScope;
} // namespace detail

template <typename T> class Signal;
template <typename T> class Computed;
class Effect;
class Batch;

// ============================================================================
// detail - Reactive runtime internals
// ============================================================================

namespace detail {

// ----------------------------------------------------------------------------
// ReactiveNode - Base for anything that can be a dependency or subscriber
// ----------------------------------------------------------------------------
// Signals are pure sources (have subscribers, no dependencies).
// Computed nodes are both (have subscribers AND dependencies).
// Effects are pure sinks (have dependencies, no subscribers).

struct ReactiveNode {
    std::vector<ReactiveNode*> subscribers;
    std::vector<ReactiveNode*> dependencies;
    uint64_t                   version = 0;
    // pending: O(1) batch-dedup flag — set when the node is queued in
    // pending_notifications, cleared when the batch flushes. Replaces the
    // O(n) std::ranges::find scan in notify_subscribers().
    bool                       pending = false;

    virtual ~ReactiveNode() = default;
    virtual void mark_dirty() = 0;
    virtual void evaluate()   = 0;

    void subscribe(ReactiveNode* node) {
        if (std::ranges::find(subscribers, node) == subscribers.end()) {
            subscribers.push_back(node);
        }
    }

    void unsubscribe(ReactiveNode* node) {
        std::erase(subscribers, node);
    }

    void clear_dependencies() {
        for (auto* dep : dependencies) {
            dep->unsubscribe(this);
        }
        dependencies.clear();
    }

    void add_dependency(ReactiveNode* dep) {
        if (std::ranges::find(dependencies, dep) == dependencies.end()) {
            dependencies.push_back(dep);
            dep->subscribe(this);
        }
    }
};

// ----------------------------------------------------------------------------
// ReactiveScope - Thread-local tracking context
// ----------------------------------------------------------------------------
// When a reactive scope is active, any Signal::get() call registers the
// signal as a dependency of the current scope's owner node.

struct ReactiveScope {
    ReactiveNode* owner;
    ReactiveScope* previous;

    explicit ReactiveScope(ReactiveNode* node) noexcept;
    ~ReactiveScope();

    ReactiveScope(const ReactiveScope&)            = delete;
    ReactiveScope& operator=(const ReactiveScope&) = delete;
};

// Thread-local pointer to the currently active reactive scope.
inline thread_local ReactiveScope* current_scope = nullptr;

// Thread-local batch depth counter. When > 0, notifications are deferred.
inline thread_local int batch_depth = 0;

// Thread-local list of nodes pending notification after batch completes.
// Wrapped in a function to avoid GCC's duplicate TLS-init symbol bug
// with `inline thread_local` non-trivially-initialized types.
inline std::vector<ReactiveNode*>& pending_notifications() {
    thread_local std::vector<ReactiveNode*> v;
    return v;
}

inline ReactiveScope::ReactiveScope(ReactiveNode* node) noexcept
    : owner(node), previous(current_scope) {
    current_scope = this;
}

inline ReactiveScope::~ReactiveScope() {
    current_scope = previous;
}

// ----------------------------------------------------------------------------
// notify_subscribers - Propagate change through the graph
// ----------------------------------------------------------------------------

inline void notify_subscribers(ReactiveNode* source) {
    if (batch_depth > 0) {
        // Defer: enqueue each subscriber once using the O(1) pending flag.
        // This replaces the old O(n) std::ranges::find scan.
        for (auto* sub : source->subscribers) {
            if (!sub->pending) {
                sub->pending = true;
                pending_notifications().push_back(sub);
            }
        }
    } else {
        // Immediate: mark all subscribers dirty then evaluate.
        // Copy the subscriber list — evaluation may modify it.
        auto subs = source->subscribers;
        for (auto* sub : subs) sub->mark_dirty();
        for (auto* sub : subs) sub->evaluate();
    }
}

// ----------------------------------------------------------------------------
// flush_batch - Run all deferred notifications
// ----------------------------------------------------------------------------

inline void flush_batch() {
    // Take ownership of the pending list so re-entrant batches work.
    auto nodes = std::move(pending_notifications());
    // Clear pending flags before evaluating so re-entrant signal sets
    // during evaluation can re-enqueue nodes correctly.
    for (auto* node : nodes) {
        node->pending = false;
        node->mark_dirty();
    }
    for (auto* node : nodes) {
        node->evaluate();
    }
}

// ----------------------------------------------------------------------------
// track - Called by Signal::get() to register dependency
// ----------------------------------------------------------------------------

inline void track(ReactiveNode* source) {
    if (current_scope && current_scope->owner) {
        current_scope->owner->add_dependency(source);
    }
}

} // namespace detail

// ============================================================================
// Signal<T> - Reactive mutable value
// ============================================================================
// The fundamental reactive primitive. Holds a value of type T. When the
// value changes, all subscribers (Computed, Effect) are notified.
//
// Usage:
//   Signal<int> count{0};
//   count.get();        // returns 0, auto-tracks in reactive scope
//   count.set(42);      // notifies all dependents
//   count.update([](int& n) { n++; });

template <typename T>
class Signal {
    struct Node final : detail::ReactiveNode {
        T value;

        template <typename... Args>
            requires std::constructible_from<T, Args...>
        explicit Node(Args&&... args) : value(std::forward<Args>(args)...) {}

        void mark_dirty() override {}  // Signals are sources - never dirty.
        void evaluate()   override {}  // Nothing to evaluate.
    };

    std::shared_ptr<Node> node_;

public:
    // Construct with a value (or default-construct T).
    Signal() requires std::default_initializable<T>
        : node_(std::make_shared<Node>()) {}

    template <typename U = T>
        requires std::constructible_from<T, U&&> &&
                 (!std::same_as<std::remove_cvref_t<U>, Signal>)
    explicit Signal(U&& initial)
        : node_(std::make_shared<Node>(std::forward<U>(initial))) {}

    // Read the current value. Automatically registers the enclosing
    // reactive scope (Computed or Effect) as a subscriber.
    [[nodiscard]] const T& get() const {
        detail::track(node_.get());
        return node_->value;
    }

    // Convenience operator for reading.
    [[nodiscard]] const T& operator()() const { return get(); }

    // Write a new value. If the value actually changed (via operator==),
    // all subscribers are notified.
    void set(const T& new_value) requires std::copyable<T> {
        if constexpr (std::equality_comparable<T>) {
            if (node_->value == new_value) return;
        }
        node_->value = new_value;
        ++node_->version;
        detail::notify_subscribers(node_.get());
    }

    void set(T&& new_value) {
        if constexpr (std::equality_comparable<T>) {
            if (node_->value == new_value) return;
        }
        node_->value = std::move(new_value);
        ++node_->version;
        detail::notify_subscribers(node_.get());
    }

    // Update in-place via a mutator function.
    // Always notifies (we cannot know if fn changed the value).
    template <std::invocable<T&> F>
    void update(F&& fn) {
        std::invoke(std::forward<F>(fn), node_->value);
        ++node_->version;
        detail::notify_subscribers(node_.get());
    }

    // Derive a Computed<U> by mapping this signal through fn.
    template <std::invocable<const T&> F>
    [[nodiscard]] auto map(F&& fn) const -> Computed<std::invoke_result_t<F, const T&>>;

    // Version counter (monotonically increasing on each change).
    [[nodiscard]] uint64_t version() const noexcept { return node_->version; }
};

// ============================================================================
// Computed<T> - Lazily evaluated derived value
// ============================================================================
// A read-only reactive value that is computed from other signals/computed
// nodes. Memoized: only recomputes when at least one dependency has changed.
//
// Usage:
//   Signal<int> width{10};
//   Signal<int> height{20};
//   auto area = computed([&] { return width.get() * height.get(); });
//   area.get(); // 200 - dependencies auto-tracked

template <typename T>
class Computed {
    struct Node final : detail::ReactiveNode {
        MAYA_DETAIL_CALLABLE<T()> compute_fn;
        T                            cached_value;
        bool                         dirty = true;

        explicit Node(MAYA_DETAIL_CALLABLE<T()> fn)
            : compute_fn(std::move(fn)), cached_value{} {}

        void mark_dirty() override {
            dirty = true;
        }

        void evaluate() override {
            if (!dirty) return;

            // Clear old dependency edges - they will be re-established
            // during re-evaluation via track().
            clear_dependencies();

            // Enter a reactive scope so that any get() calls inside
            // compute_fn register this node as a subscriber.
            detail::ReactiveScope scope(this);
            T new_value = compute_fn();

            if constexpr (std::equality_comparable<T>) {
                if (cached_value == new_value) {
                    dirty = false;
                    return;
                }
            }

            cached_value = std::move(new_value);
            dirty = false;
            ++version;

            // Propagate to our own subscribers.
            detail::notify_subscribers(this);
        }
    };

    std::shared_ptr<Node> node_;

    explicit Computed(std::shared_ptr<Node> node) : node_(std::move(node)) {}

    template <typename U> friend class Signal;

    template <std::invocable F>
        requires (!std::same_as<std::invoke_result_t<F>, void>)
    friend auto computed(F&& fn) -> Computed<std::invoke_result_t<F>>;

public:
    // Read the cached value, recomputing if dirty.
    // Auto-tracks the enclosing reactive scope.
    [[nodiscard]] const T& get() const {
        // Ensure fresh value before tracking.
        if (node_->dirty) {
            node_->evaluate();
        }
        detail::track(node_.get());
        return node_->cached_value;
    }

    [[nodiscard]] const T& operator()() const { return get(); }
};

// Signal::map implementation (needs Computed to be complete).
template <typename T>
template <std::invocable<const T&> F>
auto Signal<T>::map(F&& fn) const -> Computed<std::invoke_result_t<F, const T&>> {
    // Capture a copy of *this (shared_ptr) so the signal stays alive.
    auto self = *this;
    auto mapper = std::forward<F>(fn);
    return computed([self = std::move(self), mapper = std::move(mapper)]() {
        return mapper(self.get());
    });
}

// Factory: create a Computed from a callable.
template <std::invocable F>
    requires (!std::same_as<std::invoke_result_t<F>, void>)
auto computed(F&& fn) -> Computed<std::invoke_result_t<F>> {
    using T = std::invoke_result_t<F>;
    using Node = typename Computed<T>::Node;

    auto node = std::make_shared<Node>(std::forward<F>(fn));

    // Perform the initial evaluation so dependencies are established.
    node->evaluate();

    return Computed<T>{std::move(node)};
}

// ============================================================================
// Effect - Side-effect that re-runs when dependencies change
// ============================================================================
// RAII object. The effect is alive as long as the Effect object exists.
// When destroyed, the effect unsubscribes from all dependencies.
//
// Usage:
//   Signal<int> count{0};
//   auto fx = effect([&] { fmt::println("count = {}", count.get()); });
//   count.set(1);  // prints "count = 1"
//   // fx goes out of scope - effect is disposed

class Effect {
    struct Node final : detail::ReactiveNode {
        MAYA_DETAIL_CALLABLE<void()> effect_fn;
        bool                            dirty = true;

        explicit Node(MAYA_DETAIL_CALLABLE<void()> fn)
            : effect_fn(std::move(fn)) {}

        ~Node() override {
            clear_dependencies();
        }

        void mark_dirty() override {
            dirty = true;
        }

        void evaluate() override {
            if (!dirty) return;

            clear_dependencies();

            detail::ReactiveScope scope(this);
            effect_fn();

            dirty = false;
        }
    };

    std::shared_ptr<Node> node_;

public:
    Effect() = default;

    template <std::invocable F>
        requires std::same_as<std::invoke_result_t<F>, void>
    explicit Effect(F&& fn)
        : node_(std::make_shared<Node>(std::forward<F>(fn))) {
        // Run immediately to establish dependencies.
        node_->evaluate();
    }

    // Move-only. Copying an effect would be semantically confusing.
    Effect(Effect&&) noexcept            = default;
    Effect& operator=(Effect&&) noexcept = default;
    Effect(const Effect&)                = delete;
    Effect& operator=(const Effect&)     = delete;

    // Dispose: unsubscribe from all dependencies and release.
    void dispose() {
        if (node_) {
            node_->clear_dependencies();
            node_.reset();
        }
    }

    [[nodiscard]] bool active() const noexcept { return node_ != nullptr; }

    ~Effect() {
        dispose();
    }
};

// Factory: create an Effect from a callable.
template <std::invocable F>
    requires std::same_as<std::invoke_result_t<F>, void>
[[nodiscard]] Effect effect(F&& fn) {
    return Effect{std::forward<F>(fn)};
}

// ============================================================================
// Batch - RAII scope for coalescing signal updates
// ============================================================================
// Prevents intermediate re-renders when setting multiple signals.
// Subscribers are notified once when the outermost Batch is destroyed.
//
// Usage:
//   Signal<int> x{0}, y{0};
//   {
//       Batch batch;
//       x.set(10);  // deferred
//       y.set(20);  // deferred
//   }  // both notifications fire here, once

class Batch {
public:
    Batch() noexcept {
        ++detail::batch_depth;
    }

    ~Batch() {
        if (--detail::batch_depth == 0) {
            detail::flush_batch();
        }
    }

    Batch(const Batch&)            = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&)                 = delete;
    Batch& operator=(Batch&&)      = delete;
};

// Convenience: run a function inside a batch scope.
template <std::invocable F>
decltype(auto) batch(F&& fn) {
    Batch b;
    return std::invoke(std::forward<F>(fn));
}

} // namespace maya
