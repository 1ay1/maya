#include "maya/app/inline.hpp"

#include <algorithm>
#include <format>

namespace maya {

namespace detail {

int detect_terminal_width() noexcept {
    #ifdef __unix__
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    #endif
    return 80;
}

int detect_terminal_height() noexcept {
    #ifdef __unix__
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    #endif
    return 24;
}

void render_inline(const Element& root, int width, StylePool& pool,
                   std::string& buf, InlineState& st) {
    constexpr int kMaxHeight = 500;
    Canvas canvas{width, kMaxHeight, &pool};
    render_tree(root, canvas, pool, theme::dark, /*auto_height=*/true);

    int ch = content_height(canvas);

    int term_h = detect_terminal_height();
    int display_rows = std::min(ch, term_h);
    int skip_rows = ch - display_rows;

    // ── Row-hash comparison ────────────────────────────────────
    const int W = canvas.width();
    const uint64_t* cells = canvas.cells();

    std::vector<uint64_t> row_hashes(static_cast<size_t>(ch));
    int stable = 0;
    {
        int check = std::min(ch, st.prev_content_height);
        int prev_sz = static_cast<int>(st.prev_row_hashes.size());
        for (int y = 0; y < ch; ++y) {
            uint64_t h = 14695981039346656037ULL;
            const uint64_t* row = cells + y * W;
            for (int x = 0; x < W; ++x) {
                h ^= row[x];
                h *= 1099511628211ULL;
            }
            row_hashes[static_cast<size_t>(y)] = h;

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

    if (prev_live > 1) {
        buf += std::format("\x1b[{}A", prev_live - 1);
    }
    if (prev_live > 0) {
        buf += "\r";
    }

    if (live_rows > 0)
        serialize(canvas, pool, buf, ch, live_start);

    if (live_rows < prev_live) {
        int extra = prev_live - live_rows;
        for (int i = 0; i < extra; ++i) buf += "\r\n\x1b[2K";
        buf += std::format("\x1b[{}A", extra);
    }

    buf += ansi::reset;
    buf += ansi::sync_end;

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);

    st.prev_height = display_rows;
    st.prev_content_height = ch;
    st.prev_row_hashes = std::move(row_hashes);
}

} // namespace detail

void print(const Element& root) {
    int width = detail::detect_terminal_width() - 1;
    StylePool pool;
    std::string buf;
    detail::InlineState st;
    detail::render_inline(root, width, pool, buf, st);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print(const Element& root, int width) {
    StylePool pool;
    std::string buf;
    detail::InlineState st;
    detail::render_inline(root, width, pool, buf, st);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace maya
