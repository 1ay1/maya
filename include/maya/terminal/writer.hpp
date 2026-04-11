#pragma once
// maya::writer - Buffered synchronized terminal writer
//
// All terminal output goes through Writer. Operations are buffered as a
// sequence of RenderOps, then flushed in a single write wrapped in
// synchronized update markers (DEC mode 2026). This eliminates flicker
// and ensures atomic screen updates.
//
// Uses platform::NativeHandle and platform::io_write for cross-platform I/O.

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
#include "../platform/io.hpp"
#include "ansi.hpp"

namespace maya {

// ============================================================================
// RenderOp - individual rendering operations
// ============================================================================

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
    platform::NativeHandle handle_;
    std::vector<RenderOp> ops_;
    std::string flush_buf_;       // reused across frames to avoid alloc
    size_t reserve_hint_ = 4096;  // adaptive buffer size hint

public:
    explicit Writer(platform::NativeHandle h) noexcept : handle_(h) {}
    Writer() noexcept : handle_(platform::invalid_handle) {}

    // ========================================================================
    // Operation submission
    // ========================================================================

    void push(RenderOp op);
    void push_ops(std::span<const RenderOp> ops);

    // Convenience methods
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
    // Flush
    // ========================================================================

    [[nodiscard]] auto flush() -> Status;
    void discard() noexcept { ops_.clear(); }
    [[nodiscard]] size_t pending_count() const noexcept { return ops_.size(); }
    [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }

    // ========================================================================
    // Direct write — bypass the op queue entirely
    // ========================================================================

    [[nodiscard]] auto write_raw(std::string_view data) -> Status;

    /// Write as many bytes as possible without blocking.
    [[nodiscard]] auto write_some(std::string_view data) const -> Result<std::size_t>;

private:
    void optimize();
    static bool try_merge(RenderOp& existing, const RenderOp& incoming);
    static bool try_cancel_cursor(const RenderOp& existing, const RenderOp& incoming);
    void serialize(std::string& buf) const;
    [[nodiscard]] auto write_all(std::string_view data) const -> Status;
};

} // namespace maya
