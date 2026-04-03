#pragma once
// maya::app::context - Type-erased context map threaded through the element tree
//
// Like React's Context, this provides a way to pass values (theme, terminal
// size, user data) down the element tree without explicit prop drilling.
// Context<T> is a typed key; ContextMap stores values keyed by std::type_index.
//
// Thread-safety model: ContextMap is not thread-safe. A single ContextMap
// instance lives on the event loop thread and is passed by const reference
// during rendering. Mutations happen only between frames.

#include <any>
#include <memory>
#include <optional>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace maya {

// ============================================================================
// Context<T> - A typed context key
// ============================================================================
// Acts as a zero-size tag type for type-safe context lookups. The type T is
// the value type stored in the context map. Multiple Context<T> instances
// for the same T all refer to the same slot.
//
// Usage:
//   ContextMap ctx;
//   ctx.set<Theme>(theme::dark);
//   const Theme* t = ctx.get<Theme>();   // non-null if set

template <typename T>
struct Context {
    using value_type = T;

    // Purely a type tag -- no instance data needed.
    constexpr Context() noexcept = default;
};

// ============================================================================
// ContextMap - Type-erased storage for context values
// ============================================================================
// Internally maps std::type_index -> std::any. The type_index is derived
// from the template parameter T at each call site, guaranteeing type safety
// through the typed Context<T> accessor pattern.
//
// Supports copy-on-write semantics for cheap forking: a child scope can
// inherit from a parent without duplicating the entire map until a write
// occurs (via the shared_ptr<Impl> + cow() pattern).

class ContextMap {
    struct Impl {
        std::unordered_map<std::type_index, std::any> data;
    };

    std::shared_ptr<Impl> impl_;

    // Copy-on-write: ensure we have a unique, mutable Impl.
    void cow() {
        if (!impl_) {
            impl_ = std::make_shared<Impl>();
        } else if (impl_.use_count() > 1) {
            impl_ = std::make_shared<Impl>(*impl_);
        }
    }

public:
    ContextMap() : impl_(std::make_shared<Impl>()) {}

    // ========================================================================
    // set<T> - Store a value in the context
    // ========================================================================
    // Overwrites any previously stored value of the same type.

    template <typename T>
    void set(T value) {
        cow();
        impl_->data.insert_or_assign(
            std::type_index(typeid(T)),
            std::any(std::move(value))
        );
    }

    // ========================================================================
    // get<T> - Retrieve a value by type
    // ========================================================================
    // Returns a pointer to the stored value, or nullptr if no value of type T
    // has been set. The pointer is valid until the next mutating operation on
    // this ContextMap instance.

    template <typename T>
    [[nodiscard]] const T* get() const noexcept {
        if (!impl_) return nullptr;

        auto it = impl_->data.find(std::type_index(typeid(T)));
        if (it == impl_->data.end()) return nullptr;

        return std::any_cast<T>(&it->second);
    }

    // ========================================================================
    // get_or<T> - Retrieve a value with a fallback default
    // ========================================================================

    template <typename T>
    [[nodiscard]] const T& get_or(const T& fallback) const noexcept {
        const T* ptr = get<T>();
        return ptr ? *ptr : fallback;
    }

    // ========================================================================
    // has<T> - Check whether a value of type T is stored
    // ========================================================================

    template <typename T>
    [[nodiscard]] bool has() const noexcept {
        return get<T>() != nullptr;
    }

    // ========================================================================
    // remove<T> - Erase a value by type
    // ========================================================================
    // Returns true if a value was actually removed.

    template <typename T>
    bool remove() {
        if (!impl_) return false;
        cow();
        return impl_->data.erase(std::type_index(typeid(T))) > 0;
    }

    // ========================================================================
    // fork - Create a child context that inherits all current values
    // ========================================================================
    // The returned ContextMap shares storage with this one until either side
    // is mutated (copy-on-write). This is O(1) and ideal for passing context
    // down a subtree with local overrides.

    [[nodiscard]] ContextMap fork() const {
        ContextMap child;
        child.impl_ = impl_;
        return child;
    }

    // ========================================================================
    // clear - Remove all stored values
    // ========================================================================

    void clear() {
        cow();
        impl_->data.clear();
    }

    // ========================================================================
    // size - Number of stored values
    // ========================================================================

    [[nodiscard]] std::size_t size() const noexcept {
        return impl_ ? impl_->data.size() : 0;
    }

    // ========================================================================
    // empty - Whether the context is empty
    // ========================================================================

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }
};

} // namespace maya
