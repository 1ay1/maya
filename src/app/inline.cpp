#include "maya/app/inline.hpp"

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

int render_inline(const Element& root, int width, StylePool& pool,
                  std::string& buf, int prev_height) {
    constexpr int kMaxHeight = 500;
    Canvas canvas{width, kMaxHeight, &pool};
    render_tree(root, canvas, pool, theme::dark, /*auto_height=*/true);

    int h = content_height(canvas);

    buf.clear();
    // Move cursor to start of previous output without erasing — overwrite in place.
    if (prev_height > 1)
        buf += std::format("\x1b[{}A", prev_height - 1);
    if (prev_height > 0)
        buf += "\r";
    serialize(canvas, pool, buf, h);
    // Clear leftover lines if new output is shorter.
    for (int i = h; i < prev_height; ++i) {
        buf += "\r\n\x1b[2K";
    }

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);
    return h;
}

} // namespace detail

void print(const Element& root) {
    int width = detail::detect_terminal_width() - 1;
    StylePool pool;
    std::string buf;
    detail::render_inline(root, width, pool, buf, 0);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print(const Element& root, int width) {
    StylePool pool;
    std::string buf;
    detail::render_inline(root, width, pool, buf, 0);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace maya
