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
                   std::string& buf, LiveState& st) {
    constexpr int kMinHeight = 500;

    // Invalidate state on width change. compose_inline_frame also resets
    // its own cache on width change, but we must reallocate the canvas.
    if (st.canvas_width != width) {
        int h = std::max(kMinHeight, st.canvas.height());
        st.canvas = Canvas{width, h, &pool};
        st.canvas_width = width;
        st.frame.reset();
        // Refresh terminal height on width changes — typical resize events
        // change both dimensions together.
        st.term_h = detect_terminal_height();
    } else {
        st.canvas.set_style_pool(&pool);
    }

    if (st.term_h <= 0) st.term_h = detect_terminal_height();

    if (st.frame.prev_rows > 0) {
        st.canvas.clear_rows(st.frame.prev_rows + 4);
    } else {
        st.canvas.clear();
    }

    render_tree(root, st.canvas, pool, theme::dark, st.layout_nodes, /*auto_height=*/true);

    int ch = content_height(st.canvas);

    // Content filled the canvas — grow using the layout-computed height.
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

    if (ch <= 0) {
        st.frame.prev_rows = 0;
        return;
    }

    buf.clear();
    compose_inline_frame(st.canvas, ch, st.term_h, pool, st.frame, buf);
    if (buf.empty()) return;

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);
}

} // namespace detail

void print(const Element& root) {
    platform::ensure_utf8();
    int width = detail::detect_terminal_width() - 1;
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    detail::render_live(root, width, pool, buf, st);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print(const Element& root, int width) {
    platform::ensure_utf8();
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    detail::render_live(root, width, pool, buf, st);
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
