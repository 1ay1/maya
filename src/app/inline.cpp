#include "maya/app/inline.hpp"

#include <cstdio>

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
                      LiveState st) {
    constexpr int kMinHeight = 500;

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

    // Lazy-init the LiveState's Writer on stdout. Owning it here
    // (rather than constructing per-call) preserves the writer's
    // residue buffer across render_live invocations — critical
    // for slow ttys where a single frame may not drain in one call.
    if (!st.writer_.has_value()) {
        st.writer_.emplace(platform::stdout_handle());
    }

    // Full canvas clear every frame so every cell starts blank and any
    // cell the current paint doesn't explicitly write stays blank.
    st.canvas_.clear();

    render_tree(root, st.canvas_, pool, theme::dark, st.layout_nodes_,
                /*auto_height=*/true);

    int ch = content_height(st.canvas_);

    if (ch >= st.canvas_.height() && !st.layout_nodes_.empty()) {
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
                    st.canvas_, rows, term_h, pool, *st.writer_, true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                auto wit = arm.verify();
                if (!wit) {
                    return std::move(arm).demote_to_stale();
                }
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_,
                    *std::move(wit), true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                return lift(std::move(arm).render(
                    st.canvas_, rows, term_h, pool, *st.writer_, true));
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
    st = detail::render_live(root, width, pool, std::move(st));
    // Restore DECAWM/cursor visibility owned by InlineFrameState.
    // compose_inline_frame leaves DECAWM off across frames to save
    // bytes on slow ttys; finalize emits the restore and consumes
    // the witness chain.
    buf.clear();
    std::move(st).finalize(buf);
    if (!buf.empty()) std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print(const Element& root, int width) {
    platform::ensure_utf8();
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    st = detail::render_live(root, width, pool, std::move(st));
    buf.clear();
    std::move(st).finalize(buf);
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

    // Grow canvas if content was clipped.
    if (rows >= canvas.height() && !layout_nodes.empty()) {
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
