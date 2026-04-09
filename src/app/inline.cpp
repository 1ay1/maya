#include "maya/app/inline.hpp"

#include <algorithm>
#include <cstdio>

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

    // Reuse or recreate the canvas only when width changes.
    if (st.canvas_width != width) {
        st.canvas = Canvas{width, kMaxHeight, &pool};
        st.canvas_width = width;
        st.prev_height = 0;
        st.prev_content_height = 0;
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

    buf.clear();
    buf += ansi::sync_start;
    buf += ansi::hide_cursor;

    // If the display needs to grow, reserve space FIRST by emitting newlines.
    // This ensures that any content pushed to scrollback during the scroll
    // is from ABOVE our live area (old terminal content), not stale frame data.
    int growth = std::max(0, display_rows - st.prev_height);
    if (growth > 0) {
        for (int i = 0; i < growth; ++i) buf += '\n';
    }

    // Move cursor to the top of our display area.
    int total_lines = st.prev_height + growth;
    if (total_lines > 1) {
        ansi::write_cursor_up(buf, total_lines - 1);
    }
    if (total_lines > 0) {
        buf += '\r';
    }

    // ED 0: erase from cursor to end of screen. One escape sequence
    // clears everything — more robust than per-line erase, and is
    // exactly what Ink/log-update uses.
    buf += "\x1b[J";

    // Write new content (full serialize from skip_rows)
    if (display_rows > 0) {
        serialize(st.canvas, pool, buf, ch, skip_rows);
    }

    buf += ansi::show_cursor;
    buf += ansi::sync_end;

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);

    st.prev_height = display_rows;
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
