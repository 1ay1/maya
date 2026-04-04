#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8

#include <algorithm>
#include <cstdint>

namespace maya {

int content_height(const Canvas& canvas) noexcept {
    const int W = canvas.width();
    const int H = canvas.height();
    const uint64_t* cells = canvas.cells();

    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            auto cell = Cell::unpack(cells[y * W + x]);
            if (cell.character != U' ' && cell.character != 0) return y + 1;
        }
    }
    return 1; // at least 1 row
}

void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows) {
    const int W = canvas.width();
    const int H = (rows > 0) ? std::min(rows, canvas.height()) : canvas.height();

    if (W <= 0 || H <= 0) return;

    const uint64_t* cells = canvas.cells();
    uint16_t current_style = UINT16_MAX; // sentinel: no SGR emitted yet

    for (int y = 0; y < H; ++y) {
        if (y > 0) out += "\r\n";

        const int row_base = y * W;
        for (int x = 0; x < W; ++x) {
            const Cell cell = Cell::unpack(cells[row_base + x]);

            // Skip the second half of a wide character — already covered.
            if (cell.width == 2) [[unlikely]] continue;

            // Style transition — pre-cached SGR, single memcpy.
            if (cell.style_id != current_style) {
                out.append(pool.sgr(cell.style_id));
                current_style = cell.style_id;
            }

            // Encode the glyph — batch UTF-8 write.
            detail::encode_utf8(cell.character, out);
        }
    }

    out += ansi::reset; // reset SGR at end of frame
}

} // namespace maya
