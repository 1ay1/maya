#pragma once
// maya::writer - Buffered synchronized terminal writer
//
// All terminal output goes through Writer. Operations are buffered as a
// sequence of RenderOps, then flushed in a single syscall wrapped in
// synchronized update markers (DEC mode 2026). This eliminates flicker
// and ensures atomic screen updates.
//
// The writer performs peephole optimizations before serialization:
// - Consecutive CursorMove ops are collapsed (dx/dy added)
// - Consecutive StyleStr ops are concatenated
// - Adjacent CursorHide + CursorShow pairs cancel out

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../core/expected.hpp"
#include "../core/types.hpp"
#include "ansi.hpp"

namespace maya {

// ============================================================================
// RenderOp - individual rendering operations
// ============================================================================
// These are the atomic units of terminal output. The Writer collects them,
// optimizes the sequence, serializes to ANSI, and flushes.

namespace render_op {

struct Write {
    std::string text;
};

struct CursorMove {
    int dx = 0;
    int dy = 0;
};

struct CursorTo {
    int col; // 1-based ANSI column
};

struct StyleStr {
    std::string sgr; // pre-built SGR sequence
};

struct HyperlinkStart {
    std::string uri;
};

struct HyperlinkEnd {};

struct CursorShow {};

struct CursorHide {};

struct ClearLine {
    int count = 1; // number of lines to clear
};

struct ClearScreen {};

} // namespace render_op

using RenderOp = std::variant<
    render_op::Write,
    render_op::CursorMove,
    render_op::CursorTo,
    render_op::StyleStr,
    render_op::HyperlinkStart,
    render_op::HyperlinkEnd,
    render_op::CursorShow,
    render_op::CursorHide,
    render_op::ClearLine,
    render_op::ClearScreen
>;

// ============================================================================
// Writer - buffered, optimized terminal writer
// ============================================================================

class Writer : MoveOnly {
    int fd_;
    std::vector<RenderOp> ops_;
    size_t reserve_hint_ = 4096; // adaptive buffer size hint

public:
    explicit Writer(int fd) noexcept : fd_(fd) {}

    // ========================================================================
    // Operation submission
    // ========================================================================

    /// Append a single render operation.
    void push(RenderOp op);

    /// Append multiple render operations.
    void push_ops(std::span<const RenderOp> ops);

    // Convenience methods for common operations

    void write_text(std::string_view text);
    void move_cursor(int dx, int dy);
    void move_to_col(int col);
    void set_style(std::string_view sgr);
    void begin_hyperlink(std::string_view uri);
    void end_hyperlink();
    void show_cursor();
    void hide_cursor();
    void clear_line(int count = 1);
    void clear_screen();

    // ========================================================================
    // Flush - optimize, serialize, write atomically
    // ========================================================================

    /// Optimize the buffered operations, serialize to ANSI, wrap in
    /// synchronized update markers, and write in a single syscall.
    /// Clears the operation buffer afterward.
    [[nodiscard]] auto flush() -> Status;

    /// Discard all buffered operations without writing.
    void discard() noexcept { ops_.clear(); }

    /// Number of buffered operations.
    [[nodiscard]] size_t pending_count() const noexcept { return ops_.size(); }

    /// Whether the buffer is empty.
    [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }

    // ========================================================================
    // Direct write --- bypass the op queue entirely
    // ========================================================================
    // The fast render path (FrameBuffer::render) builds a complete ANSI frame
    // string and calls write_raw() for a single syscall. The op queue is still
    // available for ad-hoc terminal control outside render cycles.

    [[nodiscard]] auto write_raw(std::string_view data) -> Status;

    /// Write as many bytes as possible without blocking.
    /// Returns the number of bytes written (may be less than data.size() if
    /// the fd would block). Never sends recovery sequences --- the caller owns
    /// the BSU frame and resumes it on the next POLLOUT.
    [[nodiscard]] auto write_some(std::string_view data) const -> Result<std::size_t>;

private:
    // ========================================================================
    // Peephole optimizer
    // ========================================================================
    void optimize();

    /// Try to merge `incoming` into `existing`. Returns true if merged.
    static bool try_merge(RenderOp& existing, const RenderOp& incoming);

    /// Check if two adjacent cursor visibility ops cancel each other.
    static bool try_cancel_cursor(const RenderOp& existing, const RenderOp& incoming);

    // ========================================================================
    // Serializer - convert RenderOps to ANSI byte strings
    // ========================================================================
    void serialize(std::string& buf) const;

    // ========================================================================
    // Raw I/O - write entire buffer in one or more write(2) calls
    // ========================================================================
    [[nodiscard]] auto write_all(std::string_view data) const -> Status;
};

} // namespace maya
