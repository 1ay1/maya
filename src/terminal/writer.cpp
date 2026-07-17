#include "maya/terminal/writer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    #include <fcntl.h>     // F_GETFL, F_SETFL, O_NONBLOCK
#endif

#include "maya/core/expected.hpp"
#include "maya/core/overload.hpp"
#include "maya/core/types.hpp"
#include "maya/platform/io.hpp"
#include "maya/terminal/ansi.hpp"

namespace maya {

namespace detail {

[[nodiscard]] std::size_t safe_break_len(std::string_view data) noexcept {
    const std::size_t n = data.size();
    std::size_t pos  = 0;
    std::size_t safe = 0;
    while (pos < n) {
        const auto b = static_cast<unsigned char>(data[pos]);
        if (b == 0x1B) {
            // ESC — need at least the next byte to classify.
            if (pos + 1 >= n) break;
            const auto c = static_cast<unsigned char>(data[pos + 1]);
            if (c == '[') {
                // CSI … final-byte 0x40–0x7E. Parameter bytes 0x30–0x3F,
                // intermediate 0x20–0x2F, final 0x40–0x7E.
                std::size_t k = pos + 2;
                bool found_final = false;
                while (k < n) {
                    const auto cb = static_cast<unsigned char>(data[k]);
                    if (cb >= 0x40 && cb <= 0x7E) { ++k; found_final = true; break; }
                    ++k;
                }
                if (!found_final) {
                    // Reached end of buffer (or the bare "\x1b[" prefix with
                    // no bytes after it) without a CSI final byte — the
                    // sequence is incomplete. Stop here so the writer never
                    // ships a CSI split across the wire.
                    break;
                }
                pos = k;
                safe = pos;
                continue;
            }
            if (c == ']' || c == 'P' || c == '^' || c == '_' || c == 'X') {
                // OSC ']'/DCS 'P'/PM '^'/APC '_'/SOS 'X' — terminated by
                // ST (\x1b\\) or, by convention, BEL (\x07) for OSC.
                std::size_t k = pos + 2;
                bool terminated = false;
                while (k < n) {
                    const auto cb = static_cast<unsigned char>(data[k]);
                    if (cb == 0x07) { ++k; terminated = true; break; }
                    if (cb == 0x1B && k + 1 < n
                        && static_cast<unsigned char>(data[k + 1]) == '\\')
                    {
                        k += 2; terminated = true; break;
                    }
                    ++k;
                }
                if (!terminated) break;
                pos = k;
                safe = pos;
                continue;
            }
            // Two-byte ESC sequence: ESC + <final>. The final byte was
            // already in range above. Consume both.
            pos += 2;
            safe = pos;
            continue;
        }
        if (b == 0x18 || b == 0x1A) {
            // CAN / SUB cancel any in-flight control sequence. Treat
            // as a complete unit.
            ++pos;
            safe = pos;
            continue;
        }
        if (b < 0x80) {
            // Plain ASCII / C0.
            ++pos;
            safe = pos;
            continue;
        }
        // UTF-8 lead: determine sequence length from the top bits.
        std::size_t seqlen = 1;
        if      ((b & 0xE0) == 0xC0) seqlen = 2;
        else if ((b & 0xF0) == 0xE0) seqlen = 3;
        else if ((b & 0xF8) == 0xF0) seqlen = 4;
        // Continuation byte without a lead, or 5/6-byte sequences (illegal
        // in modern UTF-8): treat as a single byte to make progress.
        if (pos + seqlen > n) break;     // incomplete codepoint
        pos += seqlen;
        safe = pos;
    }
    return safe;
}

} // namespace detail

// ============================================================================
// Construction / move / destruction
// ============================================================================
// The output fd is put into non-blocking mode at Writer construction so
// a slow tty's full kernel buffer surfaces as EAGAIN rather than blocking
// the event loop. The prior flags are stashed and restored in the dtor
// so the user's shell — which shares the open file description with us —
// doesn't inherit O_NONBLOCK on stdout after agentty exits.

Writer::Writer(platform::NativeHandle h, bool nonblocking) noexcept : handle_(h) {
    ops_.reserve(kOpsReserveHint);
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    if (nonblocking && h >= 0) {
        int prev = ::fcntl(h, F_GETFL);
        if (prev >= 0 && (prev & O_NONBLOCK) == 0) {
            if (::fcntl(h, F_SETFL, prev | O_NONBLOCK) == 0) {
                prior_output_flags_ = prev;
            }
        }
    }
#endif
}

Writer::~Writer() {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    if (prior_output_flags_ >= 0 && handle_ >= 0) {
        (void)::fcntl(handle_, F_SETFL, prior_output_flags_);
    }
#endif
}

// Suspend/resume the O_NONBLOCK adjustment across a TUI suspend. The
// child spawned during suspend shares this open file description; a
// pager (less) or any bulk writer that doesn't handle EAGAIN on stdout
// would malfunction with O_NONBLOCK inherited. prior_output_flags_ >= 0
// doubles as "we own the flag change" — when the ctor never adjusted
// (Windows, fcntl failure, nonblocking=false) both calls are no-ops.
void Writer::suspend_nonblocking() noexcept {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    if (prior_output_flags_ >= 0 && handle_ >= 0)
        (void)::fcntl(handle_, F_SETFL, prior_output_flags_);
#endif
}

void Writer::resume_nonblocking() noexcept {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    if (prior_output_flags_ >= 0 && handle_ >= 0)
        (void)::fcntl(handle_, F_SETFL, prior_output_flags_ | O_NONBLOCK);
#endif
}

Writer::Writer(Writer&& other) noexcept
    : handle_(other.handle_)
    , ops_(std::move(other.ops_))
    , flush_buf_(std::move(other.flush_buf_))
    , reserve_hint_(other.reserve_hint_)
    , ns_per_byte_ema_(other.ns_per_byte_ema_)
    , residue_(std::move(other.residue_))
    , prior_output_flags_(std::exchange(other.prior_output_flags_, -1))
{
    other.handle_ = platform::invalid_handle;
}

Writer& Writer::operator=(Writer&& other) noexcept {
    if (this != &other) {
        this->~Writer();
        new (this) Writer(std::move(other));
    }
    return *this;
}

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

// ============================================================================
// Non-blocking + residue-buffered path
// ============================================================================
//
// On a tty with O_NONBLOCK set, every `::write` either succeeds in
// emitting some bytes (possibly fewer than asked) or returns EAGAIN.
// We never block. The "would have blocked" suffix lands in `residue_`
// and the next render's first action is to retry it. While residue is
// non-empty the caller is forbidden from running compose_inline_frame —
// the cell-state shadow update inside that function assumes the wire
// has actually accepted the emitted bytes, which would be a lie while
// part of the previous frame sits in our buffer.

auto Writer::try_drain_residue() -> Status {
    if (residue_.empty()) return ok();
    // Cap the physical write at the longest safe-ending prefix of
    // residue_, so even a 1-byte kernel accept can't leave the wire
    // mid-CSI. The unsafe tail stays in residue_ as raw bytes and
    // joins the next safe prefix the next time we drain (or, if
    // appended bytes complete the in-flight sequence, becomes safe
    // outright on the next safe_break_len pass).
    const std::size_t safe = detail::safe_break_len(residue_);
    if (safe == 0) {
        // The current buffer starts mid-sequence and has no safe
        // breakpoint yet — nothing to ship without risking a split.
        // Report WouldBlock so the caller polls again; appended bytes
        // will eventually complete the sequence.
        return err(Error::would_block());
    }
    std::string_view head{residue_.data(), safe};
    auto r = write_some(head);   // already loops on EINTR, stops on EAGAIN
    if (!r) return std::unexpected{r.error()};
    const std::size_t n = *r;
    if (n >= residue_.size()) {
        residue_.clear();
        return ok();
    }
    if (n > 0) residue_.erase(0, n);
    return err(Error::would_block());
}

auto Writer::write_or_buffer(std::string_view data) -> Status {
    // Drain old residue first so the new bytes don't get reordered
    // around bytes from a prior frame that haven't shipped yet.
    if (!residue_.empty()) {
        auto d = try_drain_residue();
        if (!d && d.error().kind != ErrorKind::WouldBlock) {
            return d;   // hard I/O error — caller handles
        }
        if (!residue_.empty()) {
            // Wire still backed up. Queue the new bytes behind the
            // residue so order is preserved when the buffer drains.
            residue_.append(data);
            return ok();
        }
    }
    if (data.empty()) return ok();
    // Cap the physical write at the longest safe-ending prefix of
    // `data`. Without this cap, a partial accept can leave the wire
    // mid-sequence and the next bytes (whether from this same call
    // or a future drain) join mid-stream, producing garbage. The
    // bytes beyond `safe` go straight to residue_ — they're a
    // well-formed continuation that the next drain will ship as a
    // unit.
    const std::size_t safe = detail::safe_break_len(data);
    std::string_view head = data.substr(0, safe);
    auto r = write_some(head);
    if (!r) return std::unexpected{r.error()};
    const std::size_t n = *r;
    if (n < data.size()) residue_.append(data.substr(n));
    return ok();
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
        if (write > 0 && try_collapse_cursor(ops_[write - 1], ops_[read])) {
            // Adjacent Show/Hide (either order): the SECOND op wins —
            // replace the first with it and drop the duplicate slot.
            ops_[write - 1] = ops_[read];
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

bool Writer::try_collapse_cursor(const RenderOp& existing, const RenderOp& incoming) {
    // An adjacent CursorHide/CursorShow pair (either order) collapses to
    // its SECOND op — the terminal's cursor-visibility state is just the
    // last DECTCEM it received, so only the final op in a run matters.
    //
    // The previous peephole DELETED both ops ("they cancel"), which is
    // only correct if the terminal already happened to be in the pair's
    // final state. Hide,Show starting from a hidden cursor must end
    // VISIBLE; annihilating the pair left it hidden. Same-op runs
    // (Show,Show) are also collapsed — free dedup, identical semantics.
    return std::visit(overload{
        [](const render_op::CursorHide&, const render_op::CursorShow&) { return true; },
        [](const render_op::CursorShow&, const render_op::CursorHide&) { return true; },
        [](const render_op::CursorShow&, const render_op::CursorShow&) { return true; },
        [](const render_op::CursorHide&, const render_op::CursorHide&) { return true; },
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

    // Measure wire time for the bandwidth-budget EMA. Sampled only on
    // fully-successful writes — partial / would-block cases skew the
    // average because the timer captures the blocked wait too.
    const auto t0 = std::chrono::steady_clock::now();

    while (remaining > 0) {
        auto result = platform::io_write(handle_, ptr, remaining);
        if (!result) {
            if (result.error().kind == ErrorKind::WouldBlock) {
                if (!wrote_any) return err(Error::would_block());
                // Partial write of `data` left the wire mid-sequence: the
                // accepted prefix can end inside a CSI (\x1b[12), an OSC
                // (\x1b]0;par), a UTF-8 codepoint (\xe2\x96), a DCS, etc.
                // The old recovery emitted a bare ESC next, relying on
                // the VT spec's "new ESC cancels pending CSI". Real
                // terminals (older VTE, conhost) instead PRINT the
                // orphan parameter bytes as literal cells before honouring
                // the new ESC — those cells then sit in the live frame
                // invisible to the renderer's shadow and become permanent
                // scrollback corruption when the row scrolls off.
                //
                // Use the byte-level cancellation codes from ECMA-48 §5.7
                // first: 0x18 CAN and 0x1A SUB. Spec-compliant terminals
                // discard the in-flight CS on either; non-compliant ones
                // render a visible substitute glyph instead of consuming
                // the orphan bytes as text — strictly better failure mode.
                // ST (\x1b\\) closes any in-flight OSC/DCS/APC/PM string
                // (which CAN/SUB don't terminate per §8.3.143). Then
                // close any open synchronized-output bracket and drop
                // residual SGR.
                static constexpr std::string_view recovery =
                    "\x18\x1a\x1b\\\x1b[?2026l\x1b[0m";
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

    // Update EMA. alpha = 0.125 — a single slow frame doesn't lock the
    // budget for many seconds, while a streak of slow frames still moves
    // the EMA toward the slow estimate in ~8 frames.
    if (const std::size_t total = data.size(); total > 0) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        const double sample = static_cast<double>(ns) / static_cast<double>(total);
        if (ns_per_byte_ema_ == 0.0) ns_per_byte_ema_ = sample;
        else                         ns_per_byte_ema_ = 0.125 * sample
                                                      + 0.875 * ns_per_byte_ema_;
    }
    return ok();
}

} // namespace maya
