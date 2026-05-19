// maya::CanvasWitness — Layer 0 implementation. See canvas_witness.hpp.

#include "maya/render/canvas_witness.hpp"

#include <cstdint>

namespace maya {

std::uint64_t hash_canvas_cells(const Canvas& c) noexcept {
    const auto* cells = c.cells();
    const std::size_t n = c.cell_count();
    return detail::fnv1a64(cells, n * sizeof(std::uint64_t));
}

std::uint64_t hash_canvas_caches(const Canvas& c) noexcept {
    // Hash (last_content_col(y) for every y) + max_content_row().
    // Cheap: O(H), and folds the two derived caches into one value the
    // in-diff re-check can compare against the witness-issue value.
    const int h = c.height();
    const int max_y = c.max_content_row();
    std::uint64_t seed = detail::fnv1a64(&max_y, sizeof(max_y));
    for (int y = 0; y < h; ++y) {
        const int v = c.last_content_col(y);
        seed = detail::fnv1a64(&v, sizeof(v), seed);
    }
    return seed;
}

// Slow-truth derivation. last_col_truth[y] = max x s.t. packed_cell(x,y)
// != Cell{}.pack(); max_y_truth = max y with any such cell.
//
// Disagreement with the cached values means a writer to cells_ failed to
// keep last_col_ / max_y_ in sync — either an overdraw path that didn't
// route through set()/fill() (raw pointer write), or a paint sequence
// that shrank the live extent without a clear_row(). Either way, the
// downstream diff would have read stale-high and produced ghost cells.
std::optional<CanvasWitness> verify_canvas(const Canvas& c) noexcept {
    const int w = c.width();
    const int h = c.height();
    if (w <= 0 || h <= 0) {
        // Degenerate canvas: caches are vacuously true. Issue a witness
        // with the trivial hashes so the diff fast-paths still typecheck.
        const std::uint64_t ch = hash_canvas_cells(c);
        const std::uint64_t kh = hash_canvas_caches(c);
        return CanvasWitness{&c, ch, kh};
    }

    const std::uint64_t blank = 0ULL;  // Cell{}.pack() == 0; all default fields.
    const std::uint64_t* cells = c.cells();

    int truth_max_y = -1;
    for (int y = 0; y < h; ++y) {
        int truth_last = -1;
        const std::uint64_t* row = cells + static_cast<std::ptrdiff_t>(y) * w;
        for (int x = w - 1; x >= 0; --x) {
            if (row[x] != blank) { truth_last = x; break; }
        }
        if (c.last_content_col(y) != truth_last) return std::nullopt;
        if (truth_last >= 0) truth_max_y = y;
    }
    if (c.max_content_row() != truth_max_y) return std::nullopt;

    return CanvasWitness{&c, hash_canvas_cells(c), hash_canvas_caches(c)};
}

} // namespace maya
