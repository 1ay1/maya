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
    void push(RenderOp op) {
        ops_.push_back(std::move(op));
    }

    /// Append multiple render operations.
    void push_ops(std::span<const RenderOp> ops) {
        ops_.insert(ops_.end(), ops.begin(), ops.end());
    }

    // Convenience methods for common operations

    void write_text(std::string_view text) {
        ops_.emplace_back(render_op::Write{std::string(text)});
    }

    void move_cursor(int dx, int dy) {
        ops_.emplace_back(render_op::CursorMove{dx, dy});
    }

    void move_to_col(int col) {
        ops_.emplace_back(render_op::CursorTo{col});
    }

    void set_style(std::string_view sgr) {
        ops_.emplace_back(render_op::StyleStr{std::string(sgr)});
    }

    void begin_hyperlink(std::string_view uri) {
        ops_.emplace_back(render_op::HyperlinkStart{std::string(uri)});
    }

    void end_hyperlink() {
        ops_.emplace_back(render_op::HyperlinkEnd{});
    }

    void show_cursor() {
        ops_.emplace_back(render_op::CursorShow{});
    }

    void hide_cursor() {
        ops_.emplace_back(render_op::CursorHide{});
    }

    void clear_line(int count = 1) {
        ops_.emplace_back(render_op::ClearLine{count});
    }

    void clear_screen() {
        ops_.emplace_back(render_op::ClearScreen{});
    }

    // ========================================================================
    // Flush - optimize, serialize, write atomically
    // ========================================================================

    /// Optimize the buffered operations, serialize to ANSI, wrap in
    /// synchronized update markers, and write in a single syscall.
    /// Clears the operation buffer afterward.
    [[nodiscard]] auto flush() -> Status {
        if (ops_.empty()) return ok();

        optimize();

        std::string buf;
        buf.reserve(reserve_hint_);

        // Synchronized update start
        buf += ansi::sync_start;

        // Serialize all operations
        serialize(buf);

        // Synchronized update end
        buf += ansi::sync_end;

        // Adaptive reserve hint for next frame
        reserve_hint_ = std::max(reserve_hint_, buf.size() + buf.size() / 4);

        // Single write syscall
        auto result = write_all(buf);

        ops_.clear();
        return result;
    }

    /// Discard all buffered operations without writing.
    void discard() noexcept {
        ops_.clear();
    }

    /// Number of buffered operations.
    [[nodiscard]] size_t pending_count() const noexcept {
        return ops_.size();
    }

    /// Whether the buffer is empty.
    [[nodiscard]] bool empty() const noexcept {
        return ops_.empty();
    }

    // ========================================================================
    // Direct write — bypass the op queue entirely
    // ========================================================================
    // The fast render path (FrameBuffer::render) builds a complete ANSI frame
    // string and calls write_raw() for a single syscall. The op queue is still
    // available for ad-hoc terminal control outside render cycles.

    [[nodiscard]] auto write_raw(std::string_view data) -> Status {
        return write_all(data);
    }

private:
    // ========================================================================
    // Peephole optimizer
    // ========================================================================
    // Makes a single pass over the ops, collapsing adjacent compatible
    // operations. This reduces the number of escape sequences emitted and
    // is critical for smooth rendering.

    void optimize() {
        if (ops_.size() < 2) return;

        std::vector<RenderOp> optimized;
        optimized.reserve(ops_.size());

        for (size_t i = 0; i < ops_.size(); ++i) {
            // Try to merge with the last optimized op
            if (!optimized.empty() && try_merge(optimized.back(), ops_[i])) {
                continue; // merged successfully
            }

            // Cancel CursorHide followed by CursorShow (and vice versa)
            if (!optimized.empty() && try_cancel_cursor(optimized.back(), ops_[i])) {
                optimized.pop_back(); // remove the previous op
                continue;             // skip the current op
            }

            optimized.push_back(std::move(ops_[i]));
        }

        ops_ = std::move(optimized);
    }

    /// Try to merge `incoming` into `existing`. Returns true if merged.
    static bool try_merge(RenderOp& existing, const RenderOp& incoming) {
        // Merge consecutive CursorMove ops: add dx/dy
        if (auto* a = std::get_if<render_op::CursorMove>(&existing)) {
            if (auto* b = std::get_if<render_op::CursorMove>(&incoming)) {
                a->dx += b->dx;
                a->dy += b->dy;
                return true;
            }
        }

        // Merge consecutive StyleStr ops: concatenate SGR strings
        if (auto* a = std::get_if<render_op::StyleStr>(&existing)) {
            if (auto* b = std::get_if<render_op::StyleStr>(&incoming)) {
                a->sgr += b->sgr;
                return true;
            }
        }

        // Merge consecutive Write ops: concatenate text
        if (auto* a = std::get_if<render_op::Write>(&existing)) {
            if (auto* b = std::get_if<render_op::Write>(&incoming)) {
                a->text += b->text;
                return true;
            }
        }

        // Merge consecutive ClearLine ops: add counts
        if (auto* a = std::get_if<render_op::ClearLine>(&existing)) {
            if (auto* b = std::get_if<render_op::ClearLine>(&incoming)) {
                a->count += b->count;
                return true;
            }
        }

        return false;
    }

    /// Check if two adjacent cursor visibility ops cancel each other.
    static bool try_cancel_cursor(const RenderOp& existing, const RenderOp& incoming) {
        bool hide_then_show = std::holds_alternative<render_op::CursorHide>(existing)
                           && std::holds_alternative<render_op::CursorShow>(incoming);
        bool show_then_hide = std::holds_alternative<render_op::CursorShow>(existing)
                           && std::holds_alternative<render_op::CursorHide>(incoming);
        return hide_then_show || show_then_hide;
    }

    // ========================================================================
    // Serializer - convert RenderOps to ANSI byte strings
    // ========================================================================

    void serialize(std::string& buf) const {
        for (const auto& op : ops_) {
            std::visit([&buf](const auto& v) {
                using T = std::decay_t<decltype(v)>;

                if constexpr (std::same_as<T, render_op::Write>) {
                    buf += v.text;
                }
                else if constexpr (std::same_as<T, render_op::CursorMove>) {
                    // Emit separate vertical and horizontal moves
                    if (v.dy > 0)      buf += ansi::move_down(v.dy);
                    else if (v.dy < 0) buf += ansi::move_up(-v.dy);

                    if (v.dx > 0)      buf += ansi::move_right(v.dx);
                    else if (v.dx < 0) buf += ansi::move_left(-v.dx);
                }
                else if constexpr (std::same_as<T, render_op::CursorTo>) {
                    buf += ansi::move_to_col(v.col);
                }
                else if constexpr (std::same_as<T, render_op::StyleStr>) {
                    buf += v.sgr;
                }
                else if constexpr (std::same_as<T, render_op::HyperlinkStart>) {
                    buf += ansi::hyperlink_start(v.uri);
                }
                else if constexpr (std::same_as<T, render_op::HyperlinkEnd>) {
                    buf += ansi::hyperlink_end();
                }
                else if constexpr (std::same_as<T, render_op::CursorShow>) {
                    buf += ansi::show_cursor;
                }
                else if constexpr (std::same_as<T, render_op::CursorHide>) {
                    buf += ansi::hide_cursor;
                }
                else if constexpr (std::same_as<T, render_op::ClearLine>) {
                    // Clear `count` lines: clear current, then move down and
                    // clear for each additional line.
                    buf += ansi::clear_line();
                    for (int i = 1; i < v.count; ++i) {
                        buf += ansi::move_down(1);
                        buf += ansi::clear_line();
                    }
                    // Move back up to original line if we moved
                    if (v.count > 1) {
                        buf += ansi::move_up(v.count - 1);
                    }
                }
                else if constexpr (std::same_as<T, render_op::ClearScreen>) {
                    buf += ansi::clear_screen();
                    buf += ansi::home();
                }
            }, op);
        }
    }

    // ========================================================================
    // Raw I/O - write entire buffer in one or more write(2) calls
    // ========================================================================

    [[nodiscard]] auto write_all(std::string_view data) const -> Status {
        const char* ptr  = data.data();
        auto remaining   = static_cast<ssize_t>(data.size());
        bool wrote_any   = false;

        while (remaining > 0) {
            ssize_t n = ::write(fd_, ptr, static_cast<size_t>(remaining));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!wrote_any) return err(Error::would_block()); // nothing sent — clean
                    // Partial write: the frame included sync-start but not sync-end.
                    // Terminals that implement BSU (DEC mode 2026) — foot, WezTerm — will
                    // stall waiting for the end-sync marker. Send it now so they render
                    // what they have and don't block the display for up to 100 ms.
                    static constexpr std::string_view recovery = "\x1b[?2026l\x1b[0m";
                    ::write(fd_, recovery.data(), recovery.size()); // best-effort
                    return err(Error::would_block());
                }
                return err(Error::from_errno("write"));
            }
            wrote_any  = true;
            ptr       += n;
            remaining -= n;
        }
        return ok();
    }
};

} // namespace maya
