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


void render_live(const Element& root, int width, StylePool& pool,
                   std::string& /*buf*/, LiveState& st) {
    // The Witness Chain owns its own byte buffer inside FrameBytes;
    // the legacy `buf` parameter is no longer used at this layer.
    // It is retained in the signature so the simple print()/live()
    // public API keeps source compatibility with hosts that allocate
    // their own scratch string — the parameter is just ignored.

    constexpr int kMinHeight = 500;

    if (st.canvas_width != width) {
        int h = std::max(kMinHeight, st.canvas.height());
        st.canvas = Canvas{width, h, &pool};
        st.canvas_width = width;
        // Width change invalidates the prev-frame cell grid. Demote
        // any state that holds one; Empty/Fresh/HardReset stay as-is.
        st.frame = std::visit(
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
            }, std::move(st.frame));
        st.term_h = detect_terminal_height();
    } else {
        st.canvas.set_style_pool(&pool);
    }

    if (st.term_h <= 0) st.term_h = detect_terminal_height();

    // Lazy-init the LiveState's Writer on stdout. Owning it here
    // (rather than constructing per-call) preserves the writer's
    // residue buffer across render_live invocations — critical
    // for slow ttys where a single frame may not drain in one call.
    if (!st.writer.has_value()) {
        st.writer.emplace(platform::stdout_handle());
    }

    // Full canvas clear every frame so every cell starts blank and any
    // cell the current paint doesn't explicitly write stays blank.
    st.canvas.clear();

    render_tree(root, st.canvas, pool, theme::dark, st.layout_nodes,
                /*auto_height=*/true);

    int ch = content_height(st.canvas);

    if (ch >= st.canvas.height() && !st.layout_nodes.empty()) {
        int needed = st.layout_nodes[0].computed.size.height.raw();
        if (needed > st.canvas.height()) {
            st.canvas.resize(width, needed + 8);
            st.canvas.clear();
            render_tree(root, st.canvas, pool, theme::dark,
                        st.layout_nodes, /*auto_height=*/true);
            ch = content_height(st.canvas);
        }
    }

    if (ch <= 0) return;

    const TermRows term_h = query_term_rows(platform::stdout_handle());
    const auto     rows   = content_rows(st.canvas);

    // Witness Chain dispatch — same pattern as Runtime::render.
    st.frame = std::visit(
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
                    st.canvas, rows, term_h, pool, *st.writer, true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                return lift(std::move(arm).render(
                    st.canvas, rows, term_h, pool, *st.writer, true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                auto wit = arm.verify();
                if (!wit) {
                    return std::move(arm).demote_to_stale();
                }
                return lift(std::move(arm).render(
                    st.canvas, rows, term_h, pool, *st.writer,
                    *std::move(wit), true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                return lift(std::move(arm).render(
                    st.canvas, rows, term_h, pool, *st.writer, true));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                return lift(std::move(arm).render(
                    st.canvas, rows, term_h, pool, *st.writer, true));
            }
            else {
                static_assert(std::is_same_v<T, InlineFrame<Sealed>>);
                return std::move(arm);
            }
        }, std::move(st.frame));
}

} // namespace detail

void print(const Element& root) {
    platform::ensure_utf8();
    int width = detail::detect_terminal_width();
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    detail::render_live(root, width, pool, buf, st);
    // Restore DECAWM/cursor visibility owned by InlineFrameState.
    // compose_inline_frame leaves DECAWM off across frames to save
    // bytes on slow ttys; finalize() emits the restore.
    buf.clear();
    inline_frame::finalize_coherence(std::move(st.frame), buf);
    if (!buf.empty()) std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print(const Element& root, int width) {
    platform::ensure_utf8();
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    detail::render_live(root, width, pool, buf, st);
    buf.clear();
    inline_frame::finalize_coherence(std::move(st.frame), buf);
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
