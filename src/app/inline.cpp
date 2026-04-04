#include "maya/app/inline.hpp"

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
    constexpr int kMaxHeight = 60;
    Canvas canvas{width, kMaxHeight, &pool};
    render_tree(root, canvas, pool, theme::dark);

    int h = content_height(canvas);

    buf.clear();
    if (prev_height > 0) ansi::erase_lines(prev_height, buf);
    serialize(canvas, pool, buf, h);

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
