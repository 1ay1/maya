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

struct InlineFrameState;  // forward

/// Opaque token representing "commit this many rows of the current
/// frame's prev_cells to scrollback." Constructed only via
/// `InlineFrameState::scrollback_marker(rows)`, which clamps `rows` to
/// the state's actual `prev_rows` at the time of the call. Consuming
/// the marker via `InlineFrameState::commit(marker)` is the only way
/// to advance the state's scrollback boundary through the typed path.
///
/// The marker carries no state pointer / generation — single-threaded
/// inline rendering means the marker is created and consumed in close
/// temporal proximity within the same Runtime tick. The safety the
/// type provides is "you can't construct a row count from thin air" —
/// the application has to obtain a marker from a live state, which
/// forces them to read the current `prev_rows` rather than carrying a
/// stale int around.
class ScrollbackMarker {
public:
    /// Empty marker — committing it is a no-op. Useful as a default
    /// when no scrollback advance is wanted this tick.
    constexpr ScrollbackMarker() noexcept = default;

    /// Number of rows the marker will commit when consumed.
    [[nodiscard]] constexpr int rows() const noexcept { return rows_; }

    [[nodiscard]] constexpr bool empty() const noexcept { return rows_ <= 0; }

private:
    int rows_ = 0;
    constexpr explicit ScrollbackMarker(int r) noexcept : rows_(r) {}
    friend struct InlineFrameState;
};

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

    /// Terminal-mode flags persisted across frames. compose_inline_frame
    /// flips `decawm_off`/`cursor_hidden` to true the first time it
    /// emits the corresponding sequence; subsequent frames skip the
    /// per-frame re-emission and the bracket exit. reset() clears them
    /// so the next composition re-establishes the modes from scratch
    /// (cursor position is also unknown after reset). Owners call
    /// `finalize(out)` on shutdown to restore the terminal.
    bool decawm_off    = false;
    bool cursor_hidden = false;

    void reset() noexcept {
        prev_width    = 0;
        prev_rows     = 0;
        decawm_off    = false;
        cursor_hidden = false;
    }

    /// Append the bytes needed to restore terminal state owned by this
    /// frame (cursor visibility, DECAWM). Clears the flags so further
    /// calls are no-ops. Safe to call multiple times.
    void finalize(std::string& out) {
        if (cursor_hidden) {
            out.append("\x1b[?25h");
            cursor_hidden = false;
        }
        if (decawm_off) {
            out.append("\x1b[?7h");
            decawm_off = false;
        }
    }

    /// Build a typed marker for committing `rows` rows of scrollback.
    /// Clamped to [0, prev_rows]; passing prev_rows means "commit
    /// everything that's currently considered prev frame." Returns an
    /// empty marker (no-op when consumed) when prev_rows == 0.
    [[nodiscard]] ScrollbackMarker scrollback_marker(int rows) const noexcept {
        if (rows <= 0 || prev_rows <= 0) return ScrollbackMarker{};
        return ScrollbackMarker{std::min(rows, prev_rows)};
    }

    /// Commit the marker. Shifts `prev_cells` up by marker.rows() rows
    /// and decrements `prev_rows`. A marker that targets all the rows
    /// (or more) clears the state cleanly via `reset()`.
    void commit(ScrollbackMarker marker) noexcept {
        commit_prefix(marker.rows());
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
    ///
    /// New code should prefer `scrollback_marker(rows) + commit(marker)`
    /// — the typed path forces the caller to query the live state's
    /// prev_rows rather than carrying a stale int. This signature is
    /// retained for source compatibility with existing consumers.
    void commit_prefix(int rows) noexcept;
};

/// Compose the byte stream for one inline frame into `out`.
///
/// Writes nothing (returns with `out` unchanged) when the current canvas
/// is byte-for-byte identical to the previous one. Otherwise emits the
/// minimal ANSI sequence that:
///   1. (optionally) wraps the update in DEC 2026 synchronized output,
///   2. hides the cursor,
///   3. moves to the first row that actually changed (never into
///      scrollback — rows that rolled off-screen are treated as
///      committed and skipped),
///   4. for each row in [first_changed, content_rows): emits only the
///      sub-span of cells that differ from the cached prev row (so an
///      unchanged middle row costs just \r\n, and a row whose only
///      change is one digit costs ~5 bytes — not the whole row),
///   5. erases any rows the frame shrank past (still on-screen only),
///   6. restores the cursor.
///
/// `content_rows` is the new frame's row count (typically
/// `content_height(canvas)`). `term_h` is the terminal height — used to
/// clamp cursor-up moves so we never try to "scroll back" into history.
/// `state` is updated with the new cell buffer on successful composition.
///
/// `synchronized_output` toggles the DEC 2026 wrapper. Default true for
/// backward compat; pass false on terminals that don't honor mode 2026
/// (Apple Terminal, plain xterm, tmux without `terminal-features ',sync'`).
/// The sequence is silently ignored by terminals that don't recognise it,
/// so the practical effect is byte savings rather than correctness — but
/// the answer is also a useful upstream signal for tick-rate gating.
void compose_inline_frame(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState& state,
                          std::string& out,
                          bool synchronized_output = true);

} // namespace maya
