#include "maya/app/inline.hpp"

#include <algorithm>
#include <cstdio>

#include "maya/core/simd.hpp"
#include "maya/platform/io.hpp"
#include "maya/render/diff.hpp"    // for detail::encode_utf8
#include "maya/render/renderer.hpp" // for render_tree
#include "maya/render/serialize.hpp" // for content_height, serialize

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
    constexpr int kMaxHeight = 500;

    // Invalidate state on width change.
    if (st.canvas_width != width) {
        st.canvas = Canvas{width, kMaxHeight, &pool};
        st.canvas_width = width;
        st.prev_height = 0;
        st.prev_content_height = 0;
        st.row_hashes.clear();
    } else {
        st.canvas.set_style_pool(&pool);
    }

    if (st.prev_content_height > 0) {
        st.canvas.clear_rows(st.prev_content_height + 4);
    } else {
        st.canvas.clear();
    }

    render_tree(root, st.canvas, pool, theme::dark, st.layout_nodes, /*auto_height=*/true);

    const int ch = content_height(st.canvas);
    if (ch <= 0) {
        st.prev_content_height = 0;
        return;
    }

    const int W = st.canvas.width();
    const uint64_t* cells = st.canvas.cells();
    const int term_h = detect_terminal_height();

    // Compute row hashes; swap old out.
    std::vector<uint64_t> old_hashes = std::move(st.row_hashes);
    st.row_hashes.resize(static_cast<size_t>(ch));
    for (int y = 0; y < ch; ++y)
        st.row_hashes[static_cast<size_t>(y)] =
            simd::hash_row(cells + y * W, W);

    // Row-diff: find first changed on-screen row.
    const int prev_on_screen = std::min(st.prev_height, term_h);
    const int common = std::min(ch, st.prev_height);
    const int old_hash_count = static_cast<int>(old_hashes.size());
    const int updatable_start = st.prev_height - prev_on_screen;

    int first_changed = common;
    for (int y = updatable_start; y < common && y < old_hash_count; ++y) {
        if (st.row_hashes[static_cast<size_t>(y)] !=
            old_hashes[static_cast<size_t>(y)]) {
            first_changed = y;
            break;
        }
    }

    if (first_changed == common && ch == st.prev_height) {
        st.prev_content_height = ch;
        return;
    }

    buf.clear();
    buf += ansi::sync_start;
    buf += ansi::hide_cursor;

    if (st.prev_height == 0) {
        serialize(st.canvas, pool, buf, ch);
    } else {
        int up = st.prev_height - 1 - first_changed;
        up = std::clamp(up, 0, prev_on_screen - 1);
        if (up > 0)
            ansi::write_cursor_up(buf, up);
        buf += '\r';

        serialize(st.canvas, pool, buf, ch, first_changed);

        if (ch < st.prev_height) {
            int extra = std::min(st.prev_height - ch, prev_on_screen);
            for (int i = 0; i < extra; ++i)
                buf += "\r\n\x1b[2K";
            if (extra > 0)
                ansi::write_cursor_up(buf, extra);
        }
    }

    buf += ansi::show_cursor;
    buf += ansi::sync_end;

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);

    st.prev_height = ch;
    st.prev_content_height = ch;
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
    Canvas canvas{width, 500, &pool};
    render_tree(root, canvas, pool, theme::dark, /*auto_height=*/true);

    int rows = content_height(canvas);
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
