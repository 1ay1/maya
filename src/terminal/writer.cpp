#include "maya/terminal/writer.hpp"

#include <unistd.h>

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
#include "maya/core/types.hpp"
#include "maya/terminal/ansi.hpp"

namespace maya {

// ============================================================================
// Overload helper for variant visitation
// ============================================================================

template <typename... Fs>
struct overload : Fs... { using Fs::operator()...; };

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
    auto remaining    = static_cast<ssize_t>(data.size());
    while (remaining > 0) {
        ssize_t n = ::write(fd_, ptr, static_cast<size_t>(remaining));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // would block, stop
            return err<std::size_t>(Error::from_errno("write"));
        }
        ptr       += n;
        remaining -= n;
        total     += static_cast<std::size_t>(n);
    }
    return ok(static_cast<std::size_t>(total));
}

// ============================================================================
// Peephole optimizer
// ============================================================================

void Writer::optimize() {
    if (ops_.size() < 2) return;

    auto optimized = ops_
        | std::views::all
        | std::views::transform([](auto&& op) { return std::move(op); })
        | std::ranges::to<std::vector<RenderOp>>();

    // Fold: build a new vector by merging/cancelling adjacent ops
    std::vector<RenderOp> result;
    result.reserve(optimized.size());

    for (auto& op : optimized) {
        if (!result.empty() && try_merge(result.back(), op)) {
            continue;
        }
        if (!result.empty() && try_cancel_cursor(result.back(), op)) {
            result.pop_back();
            continue;
        }
        result.push_back(std::move(op));
    }

    ops_ = std::move(result);
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
                buf += std::format("{}{}", ansi::move_down(1), ansi::clear_line());
            }
            if (v.count > 1) {
                buf += ansi::move_up(v.count - 1);
            }
        },
        [&buf](const render_op::ClearScreen&) {
            buf += std::format("{}{}", ansi::clear_screen(), ansi::home());
        }
    };

    for (const auto& op : ops_) {
        std::visit(visitor, op);
    }
}

// ============================================================================
// Raw I/O
// ============================================================================

auto Writer::write_all(std::string_view data) const -> Status {
    const char* ptr  = data.data();
    auto remaining   = static_cast<ssize_t>(data.size());
    bool wrote_any   = false;

    while (remaining > 0) {
        ssize_t n = ::write(fd_, ptr, static_cast<size_t>(remaining));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!wrote_any) return err(Error::would_block()); // nothing sent --- clean
                // Partial write: the frame included sync-start but not sync-end.
                // Terminals that implement BSU (DEC mode 2026) --- foot, WezTerm --- will
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

} // namespace maya
