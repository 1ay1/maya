#include "maya/app/inline.hpp"

#include <cstdio>

#include "maya/core/expected.hpp"  // for ErrorKind::WouldBlock (residue drain)
#include "maya/platform/io.hpp"
#include "maya/render/diff.hpp"    // for detail::encode_utf8
#include "maya/render/renderer.hpp" // for render_tree
#include "maya/render/serialize.hpp" // for content_height, serialize, compose_inline_frame

namespace maya {

namespace detail {

int detect_terminal_width() noexcept {
    auto sz = platform::query_terminal_size(platform::stdout_handle());
    return sz.width.raw() > 0 ? sz.width.raw() : 80;
}

int detect_terminal_height() noexcept {
    auto sz = platform::query_terminal_size(platform::stdout_handle());
    return sz.height.raw() > 0 ? sz.height.raw() : 24;
}


LiveState render_live(const Element& root, int width, StylePool& pool,
                      LiveState st, bool blocking) {
    constexpr int kMinHeight = 500;

    // Lazy-init the LiveState's Writer on stdout. Owning it here (rather
    // than constructing per-call) preserves the writer's residue buffer
    // across render_live invocations — critical for slow ttys where a
    // single frame may not drain in one call.
    if (!st.writer_.has_value()) {
        st.writer_.emplace(platform::stdout_handle(), /*nonblocking=*/!blocking);
    }

    // ── Backpressure via the non-blocking writer ────
    // The output fd is O_NONBLOCK. On a congested tty the previous frame
    // may have parked bytes in the writer's residue. Drain those first; if
    // the wire still won't take them, DEFER this frame — do not compose.
    // compose_inline_frame would otherwise update prev_cells to reflect a
    // frame the wire never received (breaking the diff), and — fatally for
    // a free-running animation — the residue would grow without bound:
    // a slow terminal turns a full-screen animation into a memory leak.
    // (App::render_frame's inline path already guards this way; the live()
    // loop did not, which is the leak.) A deferred frame costs one loop
    // tick of delay, not an inflated next-frame cost: prev_cells never
    // lies, so the next compose produces the same bounded diff.
    if (st.writer_->has_residue()) {
        auto d = st.writer_->try_drain_residue();
        if (!d) {
            if (d.error().kind == ErrorKind::WouldBlock) {
                return st;   // wire still backed up — skip composing this frame
            }
            // Hard I/O error: drop the now-meaningless residue and force a
            // full reset on the next successful compose.
            st.writer_->discard_residue();
            st.frame_ = std::visit(
                [](auto&& arm) -> inline_frame::InlineCoherence {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T,
                            inline_frame::InlineFrame<inline_frame::Synced>>)
                        return std::move(arm).demote_to_hard_reset();
                    else if constexpr (std::is_same_v<T,
                            inline_frame::InlineFrame<inline_frame::Stale>>)
                        return std::move(arm).escalate_to_hard_reset();
                    else
                        return std::move(arm);
                }, std::move(st.frame_));
        }
    }

    if (st.canvas_width_ != width) {
        int h = std::max(kMinHeight, st.canvas_.height());
        st.canvas_ = Canvas{width, h, &pool};
        st.canvas_width_ = width;
        // Width change invalidates the prev-frame cell grid. Demote
        // any state that holds one; Empty/Fresh/HardReset stay as-is.
        st.frame_ = std::visit(
            [](auto&& arm) -> inline_frame::InlineCoherence {
                using T = std::decay_t<decltype(arm)>;
                if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Synced>>)
                    return std::move(arm).demote_to_hard_reset();
                else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Stale>>)
                    return std::move(arm).escalate_to_hard_reset();
                else
                    return std::move(arm);
            }, std::move(st.frame_));
    } else {
        st.canvas_.set_style_pool(&pool);
    }

    // Full canvas clear every frame so every cell starts blank and any
    // cell the current paint doesn't explicitly write stays blank.
    st.canvas_.clear();

    render_tree(root, st.canvas_, pool, theme::dark, st.layout_nodes_,
                /*auto_height=*/true);

    int ch = content_height(st.canvas_);

    // Regrow on the LAYOUT's computed height, not the painted-row count:
    // blank rows at the canvas boundary keep content_height below
    // canvas.height() while the layout needs more rows, silently clipping
    // the frame's tail (see Runtime::render's "hidden chrome" rationale).
    if (!st.layout_nodes_.empty()) {
        int needed = st.layout_nodes_[0].computed.size.height.raw();
        if (needed > st.canvas_.height()) {
            st.canvas_.resize(width, needed + 8);
            st.canvas_.clear();
            render_tree(root, st.canvas_, pool, theme::dark,
                        st.layout_nodes_, /*auto_height=*/true);
            ch = content_height(st.canvas_);
        }
    }

    if (ch <= 0) return st;

    const TermRows term_h = query_term_rows(platform::stdout_handle());
    const auto     rows   = content_rows(st.canvas_);

    // DEC-2026 synchronized output. Disable with MAYA_NO_SYNC=1 for terminals
    // that mishandle it (the box renders torn/ragged). Read once per process.
    static const bool sync = (std::getenv("MAYA_NO_SYNC") == nullptr);

    // Witness Chain dispatch — same pattern as Runtime::render.
    st.frame_ = std::visit(
        [&](auto&& arm) -> inline_frame::InlineCoherence {
            using T = std::decay_t<decltype(arm)>;
            using namespace inline_frame;

            auto lift = [](auto&& outcome) -> InlineCoherence {
                return std::visit(
                    [](auto&& a) -> InlineCoherence { return std::move(a); },
                    std::move(outcome));
            };

            if constexpr (std::is_same_v<T, InlineFrame<Empty>>) {
                auto fresh = std::move(arm).seed();
                return lift(std::move(fresh).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, sync));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, sync));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                auto wit = arm.verify();
                if (!wit) {
                    return std::move(arm).demote_to_stale();
                }
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_,
                    *std::move(wit), sync));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, sync));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, sync));
            }
            else {
                static_assert(std::is_same_v<T, InlineFrame<Sealed>>);
                return std::move(arm);
            }
        }, std::move(st.frame_));

    return st;
}

} // namespace detail

void print(const Element& root) {
    platform::ensure_utf8();
    int width = detail::detect_terminal_width();
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    st = detail::render_live(root, width, pool, std::move(st), /*blocking=*/true);
    // Restore DECAWM/cursor visibility owned by InlineFrameState.
    // compose_inline_frame leaves DECAWM off across frames to save
    // bytes on slow ttys; finalize emits the restore and consumes
    // the witness chain.
    buf.clear();
    std::move(st).finalize(buf);
    // Never leave the terminal in DEC-2026 synchronized-update mode. The
    // single-frame path can emit ?2026h (sync_start) without a guaranteed
    // matching ?2026l reaching the wire; on terminals that honor ?2026
    // (iTerm2, Kitty, WezTerm, Ghostty, VS Code) an unclosed block FREEZES
    // the display until a timeout, so the printed frame looks truncated /
    // "not built well". A redundant ?2026l is a harmless no-op everywhere.
    buf += ansi::sync_end;
    if (!buf.empty()) std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print(const Element& root, int width) {
    platform::ensure_utf8();
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    st = detail::render_live(root, width, pool, std::move(st), /*blocking=*/true);
    buf.clear();
    std::move(st).finalize(buf);
    buf += ansi::sync_end;   // never leave the terminal in ?2026 sync mode
    if (!buf.empty()) std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string render_to_string(const Element& root, int width) {
    StylePool pool;
    std::vector<layout::LayoutNode> layout_nodes;
    Canvas canvas{width, 500, &pool};
    render_tree(root, canvas, pool, theme::dark, layout_nodes, /*auto_height=*/true);

    int rows = content_height(canvas);

    // Grow canvas if the layout needs more rows than the seed height.
    // Keyed on layout height, not painted rows — blank boundary rows
    // otherwise mask the overflow and clip the tail.
    if (!layout_nodes.empty()) {
        int needed = layout_nodes[0].computed.size.height.raw();
        if (needed > canvas.height()) {
            canvas.resize(width, needed + 8);
            canvas.clear();
            render_tree(root, canvas, pool, theme::dark, layout_nodes,
                        /*auto_height=*/true);
            rows = content_height(canvas);
        }
    }

    if (rows < 0) return {};

    std::string result;
    result.reserve(static_cast<std::size_t>((width + 1) * (rows + 1)));

    for (int y = 0; y <= rows; ++y) {
        // Build row string, then trim trailing spaces.
        std::size_t row_start = result.size();
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == U'\0') ch = U' ';
            detail::encode_utf8(ch, result);
        }
        // Trim trailing spaces from this row.
        std::size_t end = result.size();
        while (end > row_start && result[end - 1] == ' ') --end;
        result.resize(end);

        if (y < rows) result += '\n';
    }

    return result;
}

} // namespace maya
