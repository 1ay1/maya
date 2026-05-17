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

    // Production shadow-of-wire check: if the cells we'd diff
    // against no longer match the hash we recorded at the end of
    // the last compose, somebody scribbled on them. Force a full
    // repaint by zeroing prev_rows — same recovery shape as
    // force_redraw(), but without wiping scrollback.
    if (!verify_shadow_hash(st.frame)) {
        st.frame.ghost_rows_above = st.frame.wire_cursor_rows;
        st.frame.prev_rows     = 0;
        st.frame.cursor_hidden = false;
        st.frame.decawm_off    = false;
        st.frame.shadow_hash   = static_cast<uint64_t>(-1);
    }

    // Full canvas clear every frame so every cell starts blank and any
    // cell the current paint doesn't explicitly write stays blank. The
    // previous bounded clear (clear_rows(prev_rows + 4)) left cells past
    // its horizon retaining content from earlier frames; when a Turn's
    // height shifted between frames (streaming response, scroll, a new
    // tool panel appearing) the stale cells happened to match prev_cells
    // at those rows, so compose_inline_frame's diff stayed silent and
    // the terminal kept displaying the previous frame's content — ghost
    // composers, duplicated markdown lines, broken vertical rails. Full
    // clear costs ~6-30 µs per frame (500 × W u64 std::fill, fits in
    // L2), negligible compared to layout + paint, and eliminates the
    // whole class of stale-cell leaks.
    st.canvas.clear();

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
        // Force a full repaint on the next non-empty frame so we don't
        // diff against stale prev_cells, and drop our memory of the
        // terminal-mode escapes — if the host reset the terminal
        // between frames (signal handler, subprocess, etc.) we must
        // re-emit them on the next paint or the new frame will paint
        // with autowrap on and wide content will wrap into garbage.
        st.frame.ghost_rows_above = st.frame.wire_cursor_rows;
        st.frame.prev_rows = 0;
        st.frame.cursor_hidden = false;
        st.frame.decawm_off = false;
        return;
    }

    buf.clear();
    compose_inline_frame(st.canvas, ch, st.term_h, pool, st.frame, buf);
    if (buf.empty()) return;

    const std::size_t want = buf.size();
    const std::size_t got  = std::fwrite(buf.data(), 1, want, stdout);
    std::fflush(stdout);

    // Short write — the wire received a prefix of the frame but
    // compose_inline_frame already updated prev_cells / shadow_hash as
    // though the whole frame was delivered. Next frame's diff would
    // believe the wire matches the shadow and silently skip emitting
    // the cells that were truncated, leaving permanent corruption
    // (a half-drawn box border, a missing right edge, the classic
    // "sometimes the frame is broken" symptom).
    //
    // Recovery: invalidate the shadow and stage a force-redraw so
    // the next compose goes through case (B) and repaints the
    // viewport in place. ghost_rows_above is sized from the prior
    // emit's wire_cursor_rows so the upward erase covers the
    // (possibly larger) previous frame.
    if (got != want) {
        st.frame.ghost_rows_above = st.frame.wire_cursor_rows;
        st.frame.prev_rows = 0;
        st.frame.cursor_hidden = false;
        st.frame.decawm_off = false;
        st.frame.shadow_hash = static_cast<uint64_t>(-1);
    }
}

} // namespace detail

void print(const Element& root) {
    platform::ensure_utf8();
    int width = detail::detect_terminal_width() - 1;
    StylePool pool;
    std::string buf;
    detail::LiveState st;
    detail::render_live(root, width, pool, buf, st);
    // Restore DECAWM/cursor visibility owned by InlineFrameState.
    // compose_inline_frame leaves DECAWM off across frames to save
    // bytes on slow ttys; finalize() emits the restore.
    buf.clear();
    st.frame.finalize(buf);
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
    st.frame.finalize(buf);
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
