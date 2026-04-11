#include "maya/terminal/writer.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "maya/core/expected.hpp"
#include "maya/core/overload.hpp"
#include "maya/core/types.hpp"
#include "maya/platform/io.hpp"
#include "maya/terminal/ansi.hpp"

namespace maya {

// ============================================================================
// Operation submission
// ============================================================================

void Writer::push(RenderOp op) {
    ops_.push_back(std::move(op));
}

void Writer::push_ops(std::span<const RenderOp> ops) {
    ops_.insert(ops_.end(), ops.begin(), ops.end());
}

void Writer::write_text(std::string_view text) {
    ops_.emplace_back(render_op::Write{std::string(text)});
}

void Writer::move_cursor(int dx, int dy) {
    ops_.emplace_back(render_op::CursorMove{dx, dy});
}

void Writer::move_to_col(int col) {
    ops_.emplace_back(render_op::CursorTo{col});
}

void Writer::set_style(std::string_view sgr) {
    ops_.emplace_back(render_op::StyleStr{std::string(sgr)});
}

void Writer::begin_hyperlink(std::string_view uri) {
    ops_.emplace_back(render_op::HyperlinkStart{std::string(uri)});
}

void Writer::end_hyperlink() {
    ops_.emplace_back(render_op::HyperlinkEnd{});
}

void Writer::show_cursor() {
    ops_.emplace_back(render_op::CursorShow{});
}

void Writer::hide_cursor() {
    ops_.emplace_back(render_op::CursorHide{});
}

void Writer::clear_line(int count) {
    ops_.emplace_back(render_op::ClearLine{count});
}

void Writer::clear_screen() {
    ops_.emplace_back(render_op::ClearScreen{});
}

// ============================================================================
// Flush
// ============================================================================

auto Writer::flush() -> Status {
    if (ops_.empty()) return ok();

    optimize();

    // Reuse the member buffer across frames — clear() keeps the allocation.
    flush_buf_.clear();
    if (flush_buf_.capacity() < reserve_hint_)
        flush_buf_.reserve(reserve_hint_);

    flush_buf_ += ansi::sync_start;
    serialize(flush_buf_);
    flush_buf_ += ansi::sync_end;

    reserve_hint_ = std::max(reserve_hint_, flush_buf_.size() + flush_buf_.size() / 4);

    auto result = write_all(flush_buf_);
    ops_.clear();
    return result;
}

// ============================================================================
// Direct write
// ============================================================================

auto Writer::write_raw(std::string_view data) -> Status {
    return write_all(data);
}

auto Writer::write_some(std::string_view data) const -> Result<std::size_t> {
    if (data.empty()) return ok(std::size_t{0});

    std::size_t total = 0;
    const char* ptr   = data.data();
    std::size_t remaining = data.size();

    while (remaining > 0) {
        auto result = platform::io_write(handle_, ptr, remaining);
        if (!result) {
            if (result.error().kind == ErrorKind::WouldBlock)
                break;
            return std::unexpected{result.error()};
        }

        std::size_t n = *result;
        if (n == 0) continue;  // EINTR
        ptr       += n;
        remaining -= n;
        total     += n;
    }
    return ok(total);
}

// ============================================================================
// Peephole optimizer
// ============================================================================

void Writer::optimize() {
    if (ops_.size() < 2) return;

    // In-place compaction: read from ops_, write back compacted.
    // Avoids the extra vector allocation of the previous approach.
    std::size_t write = 0;
    for (std::size_t read = 0; read < ops_.size(); ++read) {
        if (write > 0 && try_merge(ops_[write - 1], ops_[read])) {
            continue;
        }
        if (write > 0 && try_cancel_cursor(ops_[write - 1], ops_[read])) {
            --write;
            continue;
        }
        if (write != read)
            ops_[write] = std::move(ops_[read]);
        ++write;
    }
    ops_.resize(write);
}

bool Writer::try_merge(RenderOp& existing, const RenderOp& incoming) {
    return std::visit(overload{
        [](render_op::CursorMove& a, const render_op::CursorMove& b) {
            a.dx += b.dx;
            a.dy += b.dy;
            return true;
        },
        [](render_op::StyleStr& a, const render_op::StyleStr& b) {
            a.sgr += b.sgr;
            return true;
        },
        [](render_op::Write& a, const render_op::Write& b) {
            a.text += b.text;
            return true;
        },
        [](render_op::ClearLine& a, const render_op::ClearLine& b) {
            a.count += b.count;
            return true;
        },
        [](auto&, const auto&) { return false; }
    }, existing, incoming);
}

bool Writer::try_cancel_cursor(const RenderOp& existing, const RenderOp& incoming) {
    return std::visit(overload{
        [](const render_op::CursorHide&, const render_op::CursorShow&) { return true; },
        [](const render_op::CursorShow&, const render_op::CursorHide&) { return true; },
        [](const auto&, const auto&) { return false; }
    }, existing, incoming);
}

// ============================================================================
// Serializer
// ============================================================================

void Writer::serialize(std::string& buf) const {
    auto visitor = overload{
        [&buf](const render_op::Write& v) {
            buf += v.text;
        },
        [&buf](const render_op::CursorMove& v) {
            if (v.dy > 0)      buf += ansi::move_down(v.dy);
            else if (v.dy < 0) buf += ansi::move_up(-v.dy);

            if (v.dx > 0)      buf += ansi::move_right(v.dx);
            else if (v.dx < 0) buf += ansi::move_left(-v.dx);
        },
        [&buf](const render_op::CursorTo& v) {
            buf += ansi::move_to_col(v.col);
        },
        [&buf](const render_op::StyleStr& v) {
            buf += v.sgr;
        },
        [&buf](const render_op::HyperlinkStart& v) {
            buf += ansi::hyperlink_start(v.uri);
        },
        [&buf](const render_op::HyperlinkEnd&) {
            buf += ansi::hyperlink_end();
        },
        [&buf](const render_op::CursorShow&) {
            buf += ansi::show_cursor;
        },
        [&buf](const render_op::CursorHide&) {
            buf += ansi::hide_cursor;
        },
        [&buf](const render_op::ClearLine& v) {
            buf += ansi::clear_line();
            for (int i = 1; i < v.count; ++i) {
                buf += ansi::move_down(1);
                buf += ansi::clear_line();
            }
            if (v.count > 1) {
                buf += ansi::move_up(v.count - 1);
            }
        },
        [&buf](const render_op::ClearScreen&) {
            buf += ansi::clear_screen();
            buf += ansi::home();
        }
    };

    for (const auto& op : ops_) {
        std::visit(visitor, op);
    }
}

// ============================================================================
// Raw I/O — platform-abstracted
// ============================================================================

auto Writer::write_all(std::string_view data) const -> Status {
    const char* ptr  = data.data();
    std::size_t remaining = data.size();
    bool wrote_any   = false;

    while (remaining > 0) {
        auto result = platform::io_write(handle_, ptr, remaining);
        if (!result) {
            if (result.error().kind == ErrorKind::WouldBlock) {
                if (!wrote_any) return err(Error::would_block());
                // Partial write: BSU frame open. Send recovery so terminal
                // doesn't stall waiting for sync-end.
                static constexpr std::string_view recovery = "\x1b[?2026l\x1b[0m";
                (void)platform::io_write(handle_, recovery.data(), recovery.size());
                return err(Error::would_block());
            }
            return std::unexpected{result.error()};
        }

        std::size_t n = *result;
        if (n == 0) continue;  // EINTR
        wrote_any  = true;
        ptr       += n;
        remaining -= n;
    }
    return ok();
}

} // namespace maya
