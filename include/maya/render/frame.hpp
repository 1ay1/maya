#pragma once
// maya::render::frame - Double-buffer frame system
//
// Manages two Canvas instances (front and back) that share a StylePool.
// Each render cycle:
//   1. The element tree is rendered into the back canvas.
//   2. The back canvas is diffed against the front canvas.
//   3. The frames are swapped so the freshly-rendered canvas becomes the
//      front (the "last known terminal state").
//   4. The diff result is returned as a vector of RenderOps to flush.
//
// The first frame always produces a full repaint (the front canvas starts
// blank). Subsequent frames produce minimal diffs.

#include <utility>
#include <vector>

#include "canvas.hpp"
#include "diff.hpp"
#include "renderer.hpp"
#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../style/theme.hpp"
#include "../terminal/writer.hpp"

namespace maya {

// ============================================================================
// Frame - A single frame: canvas + cursor state
// ============================================================================

struct Frame {
    Canvas   canvas;
    Position cursor  = Position::origin();
    bool     cursor_visible = true;
};

// ============================================================================
// FrameBuffer - Double-buffered rendering system
// ============================================================================
// Owns the front/back frame pair and the shared style pool.
// Thread-safety: none. Call from a single thread (the render thread).

class FrameBuffer {
public:
    FrameBuffer() = default;

    /// Construct with an initial size (terminal columns x rows).
    explicit FrameBuffer(int width, int height)
        : front_{Frame{Canvas{width, height, &style_pool_}}}
        , back_{Frame{Canvas{width, height, &style_pool_}}}
    {}

    // -- Core operations ------------------------------------------------------

    /// Render the element tree into the back buffer, diff against the front
    /// buffer, swap the frames, and return the operations needed to update
    /// the terminal.
    [[nodiscard]] std::vector<RenderOp> render(
        const Element& root,
        const Theme& theme)
    {
        // Render the tree into the back canvas.
        back_.canvas.clear();
        render_tree(root, back_.canvas, style_pool_, theme);

        // Diff the back canvas against the front canvas.
        DiffResult diff_result = diff(front_.canvas, back_.canvas, style_pool_);

        // Handle cursor visibility changes.
        if (back_.cursor_visible && !front_.cursor_visible) {
            diff_result.ops.emplace_back(render_op::CursorShow{});
        } else if (!back_.cursor_visible && front_.cursor_visible) {
            diff_result.ops.emplace_back(render_op::CursorHide{});
        }

        // If the cursor position changed, emit a final move.
        if (back_.cursor != front_.cursor && back_.cursor_visible) {
            diff_result.ops.emplace_back(render_op::Write{
                ansi::move_to(
                    back_.cursor.x.value + 1,
                    back_.cursor.y.value + 1)
            });
        }

        // Optimize the diff output.
        DiffResult optimized = optimize(std::move(diff_result));

        // Swap frames: the back (freshly rendered) becomes the new front.
        swap();

        // Reset damage on the (now-back, previously-front) canvas so the
        // next frame starts with a clean damage region.
        back_.canvas.reset_damage();

        return std::move(optimized.ops);
    }

    /// Swap front and back frames.
    void swap() {
        std::swap(front_, back_);
    }

    /// Resize both frames. This invalidates all content and forces a
    /// full repaint on the next render() call.
    void resize(int width, int height) {
        front_.canvas.resize(width, height);
        back_.canvas.resize(width, height);
    }

    // -- Cursor control -------------------------------------------------------

    /// Set the cursor position for the next frame (0-based).
    void set_cursor(Position pos) noexcept {
        back_.cursor = pos;
    }

    /// Set cursor visibility for the next frame.
    void set_cursor_visible(bool visible) noexcept {
        back_.cursor_visible = visible;
    }

    // -- Accessors ------------------------------------------------------------

    [[nodiscard]] const Frame& front() const noexcept { return front_; }
    [[nodiscard]] const Frame& back()  const noexcept { return back_; }
    [[nodiscard]] Frame& front() noexcept { return front_; }
    [[nodiscard]] Frame& back()  noexcept { return back_; }

    [[nodiscard]] StylePool& style_pool() noexcept { return style_pool_; }
    [[nodiscard]] const StylePool& style_pool() const noexcept { return style_pool_; }

    [[nodiscard]] int width()  const noexcept { return front_.canvas.width(); }
    [[nodiscard]] int height() const noexcept { return front_.canvas.height(); }

    /// Force a full repaint on the next render() call by marking the
    /// entire back canvas as damaged.
    void invalidate() {
        back_.canvas.mark_all_damaged();
    }

private:
    StylePool style_pool_;
    Frame     front_;
    Frame     back_;
};

} // namespace maya
