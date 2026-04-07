#pragma once
// maya::focus — Focus management system
//
// Thread-local, RAII-based focus scopes — same pattern as ReactiveScope.
// FocusNode registers itself with the current scope on construction and
// unregisters on destruction. Tab/Shift-Tab cycles focus within a scope.
// Nested scopes (modals) trap focus.
//
// Usage:
//   FocusNode a, b, c;
//   focus_next();          // focuses a
//   focus_next();          // focuses b
//   {
//       FocusScope modal;  // nested scope — traps focus
//       FocusNode x, y;
//       focus_next();      // focuses x within modal
//   }                      // modal destroyed — focus returns to outer scope

#include <algorithm>
#include <cstdint>
#include <vector>

#include "signal.hpp"

namespace maya {

// Forward declarations
class FocusScope;
struct FocusNode;

// ============================================================================
// Thread-local focus scope stack
// ============================================================================

inline thread_local FocusScope* current_focus_scope = nullptr;

// ============================================================================
// FocusNode — a focusable slot in the current scope
// ============================================================================

struct FocusNode {
    Signal<bool> focused{false};

    FocusNode();
    ~FocusNode();

    FocusNode(FocusNode&& o) noexcept;
    FocusNode& operator=(FocusNode&& o) noexcept;

    FocusNode(const FocusNode&)            = delete;
    FocusNode& operator=(const FocusNode&) = delete;

    /// Late-bind this node to the current focus scope (or a specific scope).
    /// Useful when the node was constructed before a scope existed.
    void bind_scope();
    void bind_scope(FocusScope& scope);

private:
    FocusScope* scope_ = nullptr;
    friend class FocusScope;
};

// ============================================================================
// FocusScope — RAII scope for focus management (nestable)
// ============================================================================

class FocusScope {
    FocusScope* parent_ = nullptr;
    std::vector<FocusNode*> nodes_;
    int active_ = -1;

public:
    FocusScope() noexcept
        : parent_(current_focus_scope) {
        current_focus_scope = this;
    }

    ~FocusScope() {
        // Blur the active node before popping
        if (active_ >= 0 && active_ < static_cast<int>(nodes_.size()))
            nodes_[static_cast<size_t>(active_)]->focused.set(false);
        current_focus_scope = parent_;
    }

    FocusScope(const FocusScope&)            = delete;
    FocusScope& operator=(const FocusScope&) = delete;
    FocusScope(FocusScope&&)                 = delete;
    FocusScope& operator=(FocusScope&&)      = delete;

    void register_node(FocusNode* node) {
        nodes_.push_back(node);
    }

    void unregister_node(FocusNode* node) {
        // If this node was focused, blur it
        auto it = std::ranges::find(nodes_, node);
        if (it != nodes_.end()) {
            int idx = static_cast<int>(it - nodes_.begin());
            node->focused.set(false);
            nodes_.erase(it);
            // Adjust active index
            if (nodes_.empty()) {
                active_ = -1;
            } else if (idx == active_) {
                active_ = active_ % static_cast<int>(nodes_.size());
                nodes_[static_cast<size_t>(active_)]->focused.set(true);
            } else if (idx < active_) {
                --active_;
            }
        }
    }

    void focus_next() {
        if (nodes_.empty()) return;
        if (active_ >= 0)
            nodes_[static_cast<size_t>(active_)]->focused.set(false);
        active_ = (active_ + 1) % static_cast<int>(nodes_.size());
        nodes_[static_cast<size_t>(active_)]->focused.set(true);
    }

    void focus_prev() {
        if (nodes_.empty()) return;
        if (active_ >= 0)
            nodes_[static_cast<size_t>(active_)]->focused.set(false);
        active_ = (active_ - 1 + static_cast<int>(nodes_.size()))
                  % static_cast<int>(nodes_.size());
        nodes_[static_cast<size_t>(active_)]->focused.set(true);
    }

    void focus_index(int idx) {
        if (idx < 0 || idx >= static_cast<int>(nodes_.size())) return;
        if (active_ >= 0)
            nodes_[static_cast<size_t>(active_)]->focused.set(false);
        active_ = idx;
        nodes_[static_cast<size_t>(active_)]->focused.set(true);
    }

    void blur_all() {
        if (active_ >= 0 && active_ < static_cast<int>(nodes_.size()))
            nodes_[static_cast<size_t>(active_)]->focused.set(false);
        active_ = -1;
    }

    [[nodiscard]] FocusNode* active() const noexcept {
        if (active_ >= 0 && active_ < static_cast<int>(nodes_.size()))
            return nodes_[static_cast<size_t>(active_)];
        return nullptr;
    }

    [[nodiscard]] int active_index() const noexcept { return active_; }
    [[nodiscard]] int size() const noexcept { return static_cast<int>(nodes_.size()); }

    friend struct FocusNode;
};

// ============================================================================
// FocusNode inline implementations
// ============================================================================

inline FocusNode::FocusNode() : scope_(current_focus_scope) {
    if (scope_) scope_->register_node(this);
}

inline FocusNode::~FocusNode() {
    if (scope_) scope_->unregister_node(this);
}

inline FocusNode::FocusNode(FocusNode&& o) noexcept
    : focused(std::move(o.focused)), scope_(o.scope_) {
    if (scope_) {
        // Replace the old pointer in the scope's node list
        auto it = std::ranges::find(scope_->nodes_, &o);
        if (it != scope_->nodes_.end()) *it = this;
    }
    o.scope_ = nullptr;
}

inline FocusNode& FocusNode::operator=(FocusNode&& o) noexcept {
    if (this != &o) {
        if (scope_) scope_->unregister_node(this);
        focused = std::move(o.focused);
        scope_ = o.scope_;
        if (scope_) {
            auto it = std::ranges::find(scope_->nodes_, &o);
            if (it != scope_->nodes_.end()) *it = this;
        }
        o.scope_ = nullptr;
    }
    return *this;
}

inline void FocusNode::bind_scope() {
    if (scope_) scope_->unregister_node(this);
    scope_ = current_focus_scope;
    if (scope_) scope_->register_node(this);
}

inline void FocusNode::bind_scope(FocusScope& scope) {
    if (scope_) scope_->unregister_node(this);
    scope_ = &scope;
    scope_->register_node(this);
}

// ============================================================================
// Free functions — operate on the current thread-local scope
// ============================================================================

inline void focus_next() {
    if (current_focus_scope) current_focus_scope->focus_next();
}

inline void focus_prev() {
    if (current_focus_scope) current_focus_scope->focus_prev();
}

} // namespace maya
