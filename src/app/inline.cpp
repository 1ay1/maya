#include "maya/app/inline.hpp"

#include <algorithm>
#include <cstdio>

#include "maya/platform/io.hpp"
#include "maya/render/diff.hpp"    // for detail::encode_utf8
#include "maya/render/renderer.hpp" // for render_tree, content_height
#include "maya/render/serialize.hpp" // for content_height

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

    // Reuse or recreate the canvas only when width changes.
    if (st.canvas_width != width) {
        // Width changed — terminal has reflowed our output, cursor position
        // is unpredictable.  Erase all previous output before resetting.
        if (st.prev_height > 0) {
            std::string erase;
            ansi::erase_lines(st.prev_height, erase);
            std::fwrite(erase.data(), 1, erase.size(), stdout);
            std::fflush(stdout);
        }
        st.canvas = Canvas{width, kMaxHeight, &pool};
        st.canvas_width = width;
        st.prev_row_hashes.clear();
        st.prev_height = 0;
        st.prev_content_height = 0;
        st.committed_height = 0;
    } else {
        st.canvas.set_style_pool(&pool);
    }
    // Partial clear: only wipe rows that had content last frame + margin.
    if (st.prev_content_height > 0) {
        st.canvas.clear_rows(st.prev_content_height + 4);
    } else {
        st.canvas.clear();
    }

    render_tree(root, st.canvas, pool, theme::dark, st.layout_nodes, /*auto_height=*/true);

    int ch = content_height(st.canvas);

    int term_h = detect_terminal_height();
    int display_rows = std::min(ch, term_h);
    int skip_rows = ch - display_rows;

    // ── Row-hash comparison (reuse scratch buffer) ────────────
    const int W = st.canvas.width();
    const uint64_t* cells = st.canvas.cells();

    st.row_hashes.resize(static_cast<size_t>(ch));
    int stable = 0;
    {
        int check = std::min(ch, st.prev_content_height);
        int prev_sz = static_cast<int>(st.prev_row_hashes.size());
        for (int y = 0; y < ch; ++y) {
            uint64_t h = simd::hash_row(cells + y * W, W);
            st.row_hashes[static_cast<size_t>(y)] = h;

            if (y == stable && y < check && y < prev_sz
                && h == st.prev_row_hashes[static_cast<size_t>(y)]) {
                stable = y + 1;
            }
        }
    }

    st.committed_height = stable;

    int live_rows = std::max(0, display_rows - std::max(0, st.committed_height - skip_rows));
    int prev_live = std::max(0, st.prev_height - std::max(0, st.committed_height - (st.prev_content_height - st.prev_height)));
    int live_start = std::max(skip_rows, st.committed_height);

    buf.clear();
    buf += ansi::sync_start;
    buf += ansi::hide_cursor;

    if (prev_live > 1) {
        ansi::write_cursor_up(buf, prev_live - 1);
    }
    if (prev_live > 0) {
        buf += "\r";
    }

    if (live_rows > 0) {
        const uint64_t* old_p = st.prev_row_hashes.data();
        int old_n = static_cast<int>(st.prev_row_hashes.size());
        serialize_changed(st.canvas, pool, buf, ch, live_start,
                          old_p, old_n,
                          st.row_hashes.data(), ch);
    }

    if (live_rows < prev_live) {
        int extra = prev_live - live_rows;
        for (int i = 0; i < extra; ++i) buf += "\r\n\x1b[2K";
        ansi::write_cursor_up(buf, extra);
    }

    buf += ansi::reset;
    buf += ansi::show_cursor;
    buf += ansi::sync_end;

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);

    st.prev_height = display_rows;
    st.prev_content_height = ch;
    std::swap(st.prev_row_hashes, st.row_hashes);
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
