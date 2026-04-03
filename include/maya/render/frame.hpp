#pragma once
// maya::render::frame - Double-buffer frame system
//
// Manages two Canvas instances (front and back) that share a StylePool.
// Each render cycle:
//   1. The element tree is rendered into the back canvas.
//   2. The back canvas is diffed against the front canvas — ANSI bytes
//      are written directly into a pre-allocated output string (no
//      intermediate RenderOp vector, no serialize pass, no optimize pass).
//   3. The output string is wrapped in synchronized-output markers so the
//      terminal renders it atomically (no tearing).
//   4. Frames are swapped; back damage is reset.
//   5. The string is returned to the caller for a single write(2) syscall.
//
// The adaptive reserve_hint_ grows the string's pre-allocated capacity
// toward the measured steady-state frame size, amortizing realloc cost.

#include <string>
#include <utility>

#include "canvas.hpp"
#include "pipeline.hpp"
#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../style/theme.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

// ============================================================================
// Frame - A single frame: canvas + cursor state
// ============================================================================

struct Frame {
    Canvas   canvas;
    Position cursor         = Position::origin();
    bool     cursor_visible = true;
};

// ============================================================================
// FrameBuffer - Double-buffered rendering system
// ============================================================================

class FrameBuffer {
public:
    FrameBuffer() = default;

    explicit FrameBuffer(int width, int height)
        : front_{Frame{Canvas{width, height, &style_pool_}}}
        , back_ {Frame{Canvas{width, height, &style_pool_}}}
    {}

    // -- Core operation -------------------------------------------------------

    /// Render the element tree, diff against the previous frame, and return
    /// a reference to an internal ANSI byte string ready to be written to the
    /// terminal in one syscall. The string includes:
    ///   cursor hide  →  sync start  →  diff ANSI  →  SGR reset  →  sync end
    ///
    /// The returned reference is valid until the next render() call.
    /// Zero per-frame heap allocations: the internal string grows once and
    /// is reused across frames (clear + refill, no dealloc/realloc).
    /// Render the element tree, diff against the previous frame, and return
    /// a reference to an internal ANSI byte string ready for a single write(2).
    ///
    /// The string is reused across frames (clear + refill preserves capacity).
    /// The type-state pipeline guarantees the render stages execute in the
    /// correct order — wrong order = compile error, not runtime assertion.
    [[nodiscard]] const std::string& render(const Element& root, const Theme& theme) {
        // Reuse the persistent output buffer: clear() preserves allocated capacity
        // so steady-state rendering is completely allocation-free.
        out_.clear();
        if (out_.capacity() < reserve_hint_) out_.reserve(reserve_hint_);

        // Execute the render pipeline as a sequence of typed state transitions.
        // Each step's requires-clause statically forbids calling it out of order.
        //
        //   Idle → Cleared → Painted → Opened → (Opened) → Closed
        //
        // The compile-time correctness proofs are in render/pipeline.hpp.
        RenderPipeline<stage::Idle>::start(back_.canvas, style_pool_, theme, out_)
            .clear()                                          // Idle    → Cleared
            .paint(root)                                      // Cleared → Painted
            .open_frame()                                     // Painted → Opened
            .write_diff(front_.canvas)                        // Opened  → Opened
            .apply_cursor(back_.cursor,  front_.cursor,       // Opened  → Opened
                          back_.cursor_visible, front_.cursor_visible)
            .close_frame();                                   // Opened  → Closed

        // Grow the reserve hint toward observed steady-state frame size.
        if (out_.size() > reserve_hint_)
            reserve_hint_ = out_.size() + out_.size() / 4;

        swap();
        back_.canvas.reset_damage();

        return out_;  // reference valid until the next render() call
    }

    /// Swap front and back frames.
    void swap() { std::swap(front_, back_); }

    /// Resize both frames. Forces a full repaint on the next render().
    void resize(int width, int height) {
        front_.canvas.resize(width, height);
        back_.canvas.resize(width, height);
    }

    // -- Cursor control -------------------------------------------------------

    void set_cursor(Position pos) noexcept         { back_.cursor = pos; }
    void set_cursor_visible(bool v) noexcept       { back_.cursor_visible = v; }

    // -- Accessors ------------------------------------------------------------

    [[nodiscard]] const Frame& front() const noexcept { return front_; }
    [[nodiscard]] const Frame& back()  const noexcept { return back_;  }
    [[nodiscard]] Frame& front() noexcept { return front_; }
    [[nodiscard]] Frame& back()  noexcept { return back_;  }

    [[nodiscard]] StylePool&       style_pool()       noexcept { return style_pool_; }
    [[nodiscard]] const StylePool& style_pool() const noexcept { return style_pool_; }

    [[nodiscard]] int width()  const noexcept { return front_.canvas.width(); }
    [[nodiscard]] int height() const noexcept { return front_.canvas.height(); }

    /// Force a full repaint on the next render() call.
    void invalidate() { back_.canvas.mark_all_damaged(); }

private:
    StylePool    style_pool_;
    Frame        front_;
    Frame        back_;
    std::string  out_;                          // persistent output buffer (reused across frames)
    std::size_t  reserve_hint_ = 4096;          // adaptive: grows toward steady-state frame size
};

} // namespace maya
