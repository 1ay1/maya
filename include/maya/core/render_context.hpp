#pragma once
// maya::core::render_context — GTK-style geometry management
//
// Provides automatic size propagation from the framework to all widgets.
// The framework sets a thread-local RenderContext before each render pass;
// widgets query available_width() / available_height() to adapt their
// layout without manual resize() calls or explicit prop drilling.
//
// CachedElement provides automatic cache invalidation: cached element trees
// are discarded whenever the render generation or available width changes,
// so widgets never serve stale layout from a previous terminal size.
//
// This is the Maya equivalent of GTK's size-request / size-allocate protocol:
//   - Widgets don't need to know about resize events
//   - The framework handles all size propagation
//   - Caches invalidate automatically on geometry changes

#include <cstdint>
#include <optional>

#include "../element/element.hpp"

namespace maya {

// ============================================================================
// RenderContext — per-frame rendering context set by the framework
// ============================================================================

struct RenderContext {
    int      width       = 80;   // available width in columns
    int      height      = 24;   // available height in rows
    uint32_t generation  = 0;    // incremented on every resize
    bool     auto_height = false; // true in inline mode (root height unconstrained)
    // Inline monotonic-height pad (composer anti-bounce). The inline
    // composer rides content_height; a transient 1-row dip in the live
    // transcript (indicator→first-content handoff, welcome→conversation
    // subtree swap, typewriter crossing a block boundary, a tool card
    // collapsing) would bounce the composer up then back down. maya sets
    // this AUTONOMOUSLY in Runtime::render: it tracks the running-max
    // content height while it fits the viewport and pads up to it across
    // a dip, then DECAYS the peak once the lower height persists (the
    // shrink is real) so idle / post-settle carries no dead space. The
    // pad is consumed by a lazy component in AppLayout::build (read at
    // paint time). 0 = no pad. NOT host-driven — set_height_hold is a
    // no-op kept only for Cmd plumbing compatibility.
    int      inline_min_content = 0;
};

// ============================================================================
// Thread-local render context (set by render_tree, read by widgets)
// ============================================================================

namespace detail {
inline thread_local const RenderContext* render_ctx_ = nullptr;
} // namespace detail

/// Query the available width from the current render context.
/// Returns 80 if called outside a render pass (safe default).
[[nodiscard]] inline int available_width() noexcept {
    return detail::render_ctx_ ? detail::render_ctx_->width : 80;
}

/// Query the available height from the current render context.
/// Returns 24 if called outside a render pass (safe default).
[[nodiscard]] inline int available_height() noexcept {
    return detail::render_ctx_ ? detail::render_ctx_->height : 24;
}

/// Host-driven inline monotonic-height floor (see RenderContext::
/// inline_min_content). Returns 0 outside a render pass / when unset.
[[nodiscard]] inline int available_inline_min_content() noexcept {
    return detail::render_ctx_ ? detail::render_ctx_->inline_min_content : 0;
}

/// Query the current render generation (incremented on resize).
/// Returns 0 if called outside a render pass.
[[nodiscard]] inline uint32_t render_generation() noexcept {
    return detail::render_ctx_ ? detail::render_ctx_->generation : 0;
}

/// True when the render pass uses auto-height (inline mode).
/// Widgets that rely on grow=1 to fill remaining space should fall
/// back to content-sized layout when this returns true.
[[nodiscard]] inline bool is_auto_height() noexcept {
    return detail::render_ctx_ && detail::render_ctx_->auto_height;
}

/// RAII guard that sets the thread-local render context for a scope.
struct RenderContextGuard {
    const RenderContext* prev_;

    explicit RenderContextGuard(const RenderContext& ctx) noexcept
        : prev_(detail::render_ctx_) {
        detail::render_ctx_ = &ctx;
    }

    ~RenderContextGuard() noexcept {
        detail::render_ctx_ = prev_;
    }

    RenderContextGuard(const RenderContextGuard&) = delete;
    RenderContextGuard& operator=(const RenderContextGuard&) = delete;
};

// ============================================================================
// CachedElement — auto-invalidating element cache
// ============================================================================
// Caches an Element tree and automatically invalidates when the render
// generation or available width changes. This replaces manual resize()
// + invalidate_cache() patterns in widgets.
//
// Usage:
//   CachedElement cache_;
//   Element build() const {
//       return cache_.get([&] { return expensive_build(); });
//   }

class CachedElement {
    mutable std::optional<Element> value_;
    mutable uint32_t gen_   = 0;
    mutable int      width_ = 0;

public:
    /// Get the cached element, rebuilding if the cache is stale.
    template <std::invocable<> F>
    [[nodiscard]] Element get(F&& builder) const {
        auto g = render_generation();
        auto w = available_width();
        if (value_ && gen_ == g && width_ == w) {
            return *value_;
        }
        value_ = builder();
        gen_   = g;
        width_ = w;
        return *value_;
    }

    /// Manually invalidate the cache (e.g. when content changes).
    void reset() noexcept { value_.reset(); }

    /// Check whether the cache currently holds a value.
    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
};

} // namespace maya
