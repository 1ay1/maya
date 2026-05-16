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

    /// EMA of nanoseconds-per-byte achieved by the underlying tty across
    /// successful writes. 0 until the first sample. Higher = slower
    /// pipe (ssh over high-latency link, slow VTE, locked-down windows
    /// console). The Runtime uses this as a bandwidth-budget signal for
    /// coalescing decisions: when ns_per_byte is high, hold small-diff
    /// frames for one tick and merge with the next.
    mutable double ns_per_byte_ema_ = 0.0;

    /// Residue carried across renders. On a slow tty the kernel write
    /// buffer fills before all bytes of a frame are accepted; the
    /// remainder lands here and gets drained by `try_drain_residue()`
    /// at the top of the next `Runtime::render`. Holding the bytes —
    /// rather than blocking the event loop on `write()` — keeps
    /// keystroke handling responsive when the wire is congested. The
    /// caller (Runtime) MUST refuse to compose a new frame while
    /// `has_residue()` is true: a new compose_inline_frame would update
    /// prev_cells to reflect a frame the wire hasn't received yet,
    /// breaking the next diff's correctness. See the comment in
    /// `Runtime::render` for the drain-before-compose protocol.
    std::string residue_;

    /// The fd's original `F_GETFL` flags at Writer construction. Set
    /// once in the constructor and restored in the destructor so the
    /// user's shell doesn't inherit `O_NONBLOCK` on stdout. -1 means
    /// "we never adjusted flags" (e.g. Windows path, or the fcntl
    /// failed at startup); the destructor leaves the fd alone.
    int prior_output_flags_ = -1;

    // A typical frame pushes 50–150 ops (one per style change + text run in
    // a view of moderate depth). Reserving up front avoids the geometric
    // reallocation chain on the first frame, and from frame 2 onward
    // ops_.clear() preserves capacity so subsequent pushes hit the fast
    // path. Cheap — sizeof(RenderOp) is ~40B, 128 * 40 = 5 KB per Writer.
    static constexpr std::size_t kOpsReserveHint = 128;

public:
    explicit Writer(platform::NativeHandle h) noexcept;
    Writer() noexcept : handle_(platform::invalid_handle) {
        ops_.reserve(kOpsReserveHint);
    }
    ~Writer();
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;

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

    /// Non-blocking, residue-buffered write. Drains any pending residue
    /// first, then attempts the data. Whatever can't fit gets appended
    /// to residue and returned as `ok()` — the caller treats the bytes
    /// as committed in order. The contract:
    ///
    ///   - returns `ok()` on full delivery OR partial delivery with the
    ///     unwritten suffix safely stashed in residue;
    ///   - returns `would_block` only when residue had to grow AND the
    ///     caller asked via `has_residue()`-style polling (current impl
    ///     never returns would_block here — it always succeeds);
    ///   - returns hard error on real I/O failure (the caller should
    ///     transition to Divergent and discard the residue via
    ///     `discard_residue()`).
    ///
    /// MUST be paired with `try_drain_residue()` at the start of the
    /// next render; otherwise the caller risks composing a new frame
    /// whose prev_cells reflects bytes still sitting in residue.
    [[nodiscard]] auto write_or_buffer(std::string_view data) -> Status;

    /// Attempt to drain pending residue without blocking. Returns
    /// `ok()` if residue is now empty (safe to compose a new frame),
    /// `would_block` if some residue remains (caller should defer the
    /// current render and try again next tick), hard error otherwise.
    [[nodiscard]] auto try_drain_residue() -> Status;

    /// True iff the writer is holding undelivered bytes from a prior
    /// non-blocking write. A frame composer MUST check this and defer
    /// before calling compose_inline_frame, since a fresh compose
    /// would update prev_cells to reflect bytes that haven't actually
    /// reached the wire yet.
    [[nodiscard]] bool has_residue() const noexcept { return !residue_.empty(); }

    /// Discard pending residue without writing. Use ONLY when the
    /// caller has just dropped its cell-cache (e.g. transitioning
    /// to Divergent after a hard I/O error); the residue's bytes
    /// reference cell positions that no longer have a defined
    /// "prev" baseline.
    void discard_residue() noexcept { residue_.clear(); }

    /// EMA of ns-per-byte measured across recent `write_all` calls. 0
    /// before the first sample lands. Use as a cheap "is the wire slow?"
    /// signal — values above kSlowTtyThreshold suggest deferring small
    /// diffs.
    [[nodiscard]] double ns_per_byte() const noexcept { return ns_per_byte_ema_; }

private:
    void optimize();
    static bool try_merge(RenderOp& existing, const RenderOp& incoming);
    static bool try_cancel_cursor(const RenderOp& existing, const RenderOp& incoming);
    void serialize(std::string& buf) const;
    [[nodiscard]] auto write_all(std::string_view data) const -> Status;
};

namespace detail {

/// Return the longest prefix of `data` that ends at a byte boundary
/// safe to leave the wire on — not inside a UTF-8 multi-byte
/// codepoint, not inside any ESC-introduced control sequence (CSI /
/// OSC / DCS / APC / PM / SOS / two-byte ESC), not on a lone
/// continuation byte. Used by Writer's residue path to guarantee
/// every kernel-bound write ends at a complete unit, so a partial
/// accept can never strand the wire mid-sequence (the historical
/// source of the "orphan parameter bytes printed as text" corruption
/// the older recovery sequence had to clean up).
///
/// Exposed under `detail::` for testability; production code should
/// not call it directly — Writer applies it at the right moments
/// (write_or_buffer, try_drain_residue) so callers don't have to
/// think about it.
[[nodiscard]] std::size_t safe_break_len(std::string_view data) noexcept;

} // namespace detail

} // namespace maya
