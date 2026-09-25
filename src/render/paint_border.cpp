// src/render/paint_border.cpp — box borders and their titles.
#include "render_internal.hpp"

namespace maya {
namespace render_detail {

// ============================================================================
// Border painting
// ============================================================================

void paint_border(
    Canvas& canvas,
    const BorderConfig& border,
    const Rect& rect,
    uint16_t style_id)
{
    if (border.empty()) return;

    auto cp = get_border_codepoints(border.style);
    int x0 = rect.left().value;
    int y0 = rect.top().value;
    int x1 = rect.right().value - 1;  // inclusive right
    int y1 = rect.bottom().value - 1; // inclusive bottom

    if (x0 > x1 || y0 > y1) return;

    // Corners — direct codepoint write, no UTF-8 round-trip.
    if (border.sides.top && border.sides.left)
        canvas.set(x0, y0, cp.top_left, style_id);
    if (border.sides.top && border.sides.right)
        canvas.set(x1, y0, cp.top_right, style_id);
    if (border.sides.bottom && border.sides.left)
        canvas.set(x0, y1, cp.bottom_left, style_id);
    if (border.sides.bottom && border.sides.right)
        canvas.set(x1, y1, cp.bottom_right, style_id);

    // Edge loops: extend to the corner cells WHEN no corner is drawn
    // there. A corner is only drawn when BOTH adjacent sides are
    // present (see the conditions above). If a corner is absent, the
    // edge owns that cell — otherwise the cell is just blank, which
    // visibly fragments left-only / right-only / top-only / bottom-only
    // borders (e.g. the Turn widget's bold left rail: top/bottom/right
    // are all false, so rows y0 and y1 had no glyph at all, producing
    // the missing-row gap at the top and bottom of every speaker turn).

    // Top edge — extends to (x0, y0) when no left, to (x1, y0) when no right.
    if (border.sides.top) {
        const int x_start = border.sides.left  ? x0 + 1 : x0;
        const int x_end   = border.sides.right ? x1     : x1 + 1;
        for (int x = x_start; x < x_end; ++x)
            canvas.set(x, y0, cp.top, style_id);
    }

    // Bottom edge — symmetric to top.
    if (border.sides.bottom) {
        const int x_start = border.sides.left  ? x0 + 1 : x0;
        const int x_end   = border.sides.right ? x1     : x1 + 1;
        for (int x = x_start; x < x_end; ++x)
            canvas.set(x, y1, cp.bottom, style_id);
    }

    // Left edge — extends to (x0, y0) when no top, to (x0, y1) when no bottom.
    if (border.sides.left) {
        const int y_start = border.sides.top    ? y0 + 1 : y0;
        const int y_end   = border.sides.bottom ? y1     : y1 + 1;
        for (int y = y_start; y < y_end; ++y)
            canvas.set(x0, y, cp.left, style_id);
    }

    // Right edge — symmetric to left.
    if (border.sides.right) {
        const int y_start = border.sides.top    ? y0 + 1 : y0;
        const int y_end   = border.sides.bottom ? y1     : y1 + 1;
        for (int y = y_start; y < y_end; ++y)
            canvas.set(x1, y, cp.right, style_id);
    }

    // Border title text (still uses write_text — rare path, variable-length string).
    auto paint_btext = [&](const BorderText& bt) {
        if (bt.content.empty()) return;
        int edge_y = (bt.position == BorderTextPos::Top) ? y0 : y1;
        int avail = x1 - x0 - 1;
        if (avail <= 0) return;

        int text_width = string_width(bt.content);
        int display_width = std::min(text_width, avail);

        // A title wider than the border span used to write its FULL content
        // and overrun the right edge — obliterating the corner glyph and
        // spilling into the neighbouring cell (a broken, unclosed box on a
        // narrow panel). Truncate to the available span so the frame always
        // closes; the ellipsis lands one cell inside the right corner.
        std::string clipped;
        std::string_view content = bt.content;
        if (text_width > avail) {
            clipped = truncate_end(bt.content, avail);
            content = clipped;
            display_width = std::min<int>(string_width(clipped), avail);
        }

        int text_x = [&] {
            switch (bt.align) {
                case BorderTextAlign::Start:
                    return x0 + 1 + bt.offset;
                case BorderTextAlign::Center:
                    return x0 + 1 + (avail - display_width) / 2 + bt.offset;
                case BorderTextAlign::End:
                    return x1 - display_width + bt.offset;
            }
            std::unreachable();
        }();

        text_x = std::clamp(text_x, x0 + 1, x1 - 1);
        canvas.write_text(text_x, edge_y, content, style_id);
    };

    if (border.text.has_value())     paint_btext(*border.text);
    if (border.text_end.has_value()) paint_btext(*border.text_end);
}

} // namespace render_detail
} // namespace maya
