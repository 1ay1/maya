#pragma once
// maya::render::serialize - Canvas serialization for inline rendering
//
// Converts canvas cells into an ANSI byte stream for terminal output.
// Used by both the one-shot `serialize()` (full canvas dump) and the
// stateful `compose_inline_frame()` (differential row update for
// Claude-Code-style inline progress output).
//
// The inline renderer keeps a cached copy of the last-rendered cells
// in `InlineFrameState`. Each frame, it compares the new canvas row-by-
// row using `simd::bulk_eq` (exact 64-bit-packed cell comparison — no
// hash collisions) to find the first row that actually changed, then
// rewrites only that row and everything below it. Rows still on-screen
// are overwritten in place; rows that scrolled into history stay put.

#include <string>

#include "canvas.hpp"

namespace maya {

/// Find the last non-empty row in a canvas (1-based height).
/// Returns the number of rows that contain visible content.
int content_height(const Canvas& canvas) noexcept;

/// Serialize `rows` rows of the canvas starting at `start_row`
/// (or all rows if rows <= 0).
void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows = 0, int start_row = 0);

// ============================================================================
// Inline frame composition
// ============================================================================

/// Persistent state for the inline (row-diff) renderer.
///
/// Holds a copy of the last-rendered cell buffer so successive frames can
/// be compared exactly via `simd::bulk_eq` instead of hashed (no collisions).
/// Carry the same instance across frames; call `reset()` to invalidate
/// after a write failure, `\x1b[2J` clear, or any other event that
/// desynchronises the terminal from the cached state.
struct InlineFrameState {
    AlignedBuffer prev_cells;
    int           prev_width = 0;
    int           prev_rows  = 0;  // content_rows from the last composed frame

    void reset() noexcept {
        prev_width = 0;
        prev_rows  = 0;
    }

    /// Mark the top `rows` rows of the current prev frame as committed to
    /// terminal scrollback.  Shifts `prev_cells` up by `rows * prev_width`
    /// and decrements `prev_rows`.
    ///
    /// Use this when the caller knows the next frame's tree intentionally
    /// omits content that was at the top of the previous frame (e.g. a
    /// chat UI that slices old messages once they've scrolled above the
    /// viewport).  Without this call, `compose_inline_frame` would treat
    /// the shorter tree as "content removed from the bottom" and erase
    /// visible rows.
    void commit_prefix(int rows) noexcept;
};

/// Compose the byte stream for one inline frame into `out`.
///
/// Writes nothing (returns with `out` unchanged) when the current canvas
/// is byte-for-byte identical to the previous one. Otherwise emits the
/// minimal ANSI sequence that:
///   1. wraps the update in DEC 2026 synchronized output,
///   2. hides the cursor,
///   3. moves to the first row that actually changed (never into
///      scrollback — rows that rolled off-screen are treated as
///      committed and skipped),
///   4. rewrites from there down through the new content,
///   5. erases any rows the frame shrank past (still on-screen only),
///   6. restores the cursor.
///
/// `content_rows` is the new frame's row count (typically
/// `content_height(canvas)`). `term_h` is the terminal height — used to
/// clamp cursor-up moves so we never try to "scroll back" into history.
/// `state` is updated with the new cell buffer on successful composition.
void compose_inline_frame(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState& state,
                          std::string& out);

} // namespace maya
