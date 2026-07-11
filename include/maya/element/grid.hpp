#pragma once
// maya::grid — responsive layout with ONE number.
//
//   grid({cpu, mem, net, disk}, 26);
//
// "Each cell wants about 26 columns." That is the entire API. maya fits as
// many cells per row as the REAL slot width allows, wraps the rest into new
// rows, and stacks everything in one column on a narrow terminal — re-solved
// live on every resize. No breakpoints to memorize, no span arithmetic, no
// per-tier declarations that drift apart: the cell count per row falls out
// of the width you already know (how wide does one cell need to be to look
// right?).
//
//   sidebar(stats, table, 42);
//
// The other layout every dashboard needs: a fixed-width rail next to a main
// pane that takes the rest — and the pair stacks vertically the moment the
// terminal is too narrow for both. Again one number: the rail's width.
//
//   col({ row({cpu, mem, net, disk}), table });
//
// And the GTK mental model for whole pages: row() puts cells side by side
// sharing the width equally — and wraps, then stacks, by itself when the
// slot narrows. col() stacks cells, each stretched to the full width.
// Everything fills automatically; nothing needs a hand-computed width.
//
// Compose them and you have a full three-shape dashboard in two lines:
//
//   sidebar(grid({cpu, mem, net, disk}, 24), table, {.width = 42});
//
//   * ultrawide  — 42-cell rail (stats stacked 1-across inside it, because
//                  the grid re-solves from its SLOT width, not the screen),
//                  table fills the rest
//   * medium    — stats flow 2-, 3-, 4-across over a full-width table
//   * narrow    — everything in one column
//
// Semantics worth knowing:
//   * Cells in a row share the width EXACTLY (largest-remainder split — no
//     ragged right edge from integer division).
//   * A short last row keeps the same cell width as the full rows above it,
//     so columns line up down the whole grid.
//   * grid re-solves from the width of the slot it SITS IN (adapt() under
//     the hood) — a grid inside a sidebar collapses independently of one in
//     the main pane.
//   * Anything fancier (a cell that spans two columns, tier-specific
//     hiding) is what adapt()/responsive() are for. The grid stays simple.

#include "builder.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace maya {

// ============================================================================
// grid()
// ============================================================================

struct GridOpts {
    int  min       = 24;     ///< a cell's comfortable minimum width (columns)
    int  max_cols  = 0;      ///< cap cells-per-row; 0 = as many as fit
    int  gap_x     = 1;      ///< blank columns between cells
    int  gap_y     = 0;      ///< blank rows between rows
    bool grow_rows = false;  ///< rows share surplus height (definite slot)
};

/// Auto-flow grid: as many `min`-wide cells per row as fit, wrap the rest,
/// one column when narrow. `grid(cells, 26)` is the whole call.
[[nodiscard]] inline auto grid(std::vector<Element> cells, GridOpts opts = {})
    -> ComponentBuilder
{
    return detail::adapt([cells = std::move(cells), opts](int w) -> Element {
        const int n = static_cast<int>(cells.size());
        if (n == 0) return Element{ElementList{}};

        const int min   = std::max(1, opts.min);
        const int gap_x = std::max(0, opts.gap_x);

        // How many min-wide cells (plus gaps between them) fit in w?
        int cols = (w + gap_x) / (min + gap_x);
        cols = std::clamp(cols, 1, n);
        if (opts.max_cols > 0) cols = std::min(cols, opts.max_cols);

        // Split the row width exactly: base + largest-remainder spread, so
        // the last cell ends flush with the slot edge.
        const int total = std::max(cols, w - gap_x * (cols - 1));
        const int base  = total / cols;
        const int rem   = total % cols;
        std::vector<int> cw(static_cast<std::size_t>(cols));
        for (int i = 0; i < cols; ++i)
            cw[static_cast<std::size_t>(i)] = base + (i < rem ? 1 : 0);

        // Wrap into rows. A short last row keeps the same cell widths so
        // columns line up down the grid.
        std::vector<Element> row_els;
        row_els.reserve(static_cast<std::size_t>((n + cols - 1) / cols));
        for (int start = 0; start < n; start += cols) {
            const int count = std::min(cols, n - start);
            std::vector<Element> cell_els;
            cell_els.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                // A Column-direction box with a fixed width: the default
                // cross-axis Stretch hands the content the FULL cell width,
                // and the row's own Stretch hands it the row height.
                auto cb = detail::vstack();
                cb.width(Dimension::fixed(cw[static_cast<std::size_t>(i)]));
                cell_els.push_back(
                    cb(Element{cells[static_cast<std::size_t>(start + i)]}));
            }
            auto rb = detail::hstack();
            if (gap_x > 0) rb.gap(gap_x);
            if (opts.grow_rows) rb.grow(1.0f);
            row_els.push_back(rb(std::move(cell_els)));
        }

        if (row_els.size() == 1) return std::move(row_els.front());
        auto vb = detail::vstack();
        if (opts.gap_y > 0) vb.gap(opts.gap_y);
        return vb(std::move(row_els));
    });
}

/// Sugar: `grid(cells, 26)` — "each cell wants about 26 columns".
[[nodiscard]] inline auto grid(std::vector<Element> cells, int min_width)
    -> ComponentBuilder
{
    return grid(std::move(cells), GridOpts{.min = min_width});
}

// ============================================================================
// sidebar()
// ============================================================================

struct SidebarOpts {
    int  width       = 32;    ///< the rail's fixed width (columns)
    int  stack_below = 0;     ///< stack when slot < this; 0 = auto (2×width:
                              ///< side-by-side only while main ≥ the rail)
    int  gap         = 1;     ///< blank columns between rail and main
    bool right       = false; ///< rail on the right instead of the left
};

/// Fixed-width rail beside a main pane that takes the rest; the pair stacks
/// vertically (reading order preserved) when the slot is too narrow.
[[nodiscard]] inline auto sidebar(Element rail, Element main,
                                  SidebarOpts opts = {}) -> ComponentBuilder
{
    return detail::adapt(
        [rail = std::move(rail), main = std::move(main), opts](int w) -> Element {
            const int rail_w = std::max(1, opts.width);
            const int threshold =
                opts.stack_below > 0 ? opts.stack_below : rail_w * 2;

            if (w >= threshold) {
                auto rb = detail::vstack();
                rb.width(Dimension::fixed(rail_w));
                auto mb = detail::vstack();
                mb.grow(1.0f);
                auto rowb = detail::hstack();
                if (opts.gap > 0) rowb.gap(opts.gap);
                std::vector<Element> kids;
                if (opts.right) {
                    kids.push_back(mb(Element{main}));
                    kids.push_back(rb(Element{rail}));
                } else {
                    kids.push_back(rb(Element{rail}));
                    kids.push_back(mb(Element{main}));
                }
                return rowb(std::move(kids));
            }

            // Too narrow: stack, preserving reading order (left → top).
            auto mainb = detail::vstack();
            mainb.grow(1.0f);
            auto colb = detail::vstack();
            std::vector<Element> kids;
            if (opts.right) {
                kids.push_back(mainb(Element{main}));
                kids.push_back(Element{rail});
            } else {
                kids.push_back(Element{rail});
                kids.push_back(mainb(Element{main}));
            }
            return colb(std::move(kids));
        });
}

/// Sugar: `sidebar(rail, main, 42)` — "42-column rail, main takes the rest".
[[nodiscard]] inline auto sidebar(Element rail, Element main, int width)
    -> ComponentBuilder
{
    return sidebar(std::move(rail), std::move(main), SidebarOpts{.width = width});
}

// ============================================================================
// row() / col() — boxes that keep themselves correct
// ============================================================================

/// Cells side by side, sharing the width equally and exactly — wrapping,
/// then stacking, by itself as the slot narrows. Same engine as grid();
/// the name reads better when composing pages: col({ row({a, b}), table }).
[[nodiscard]] inline auto row(std::vector<Element> cells, int min_width = 24)
    -> ComponentBuilder
{
    return grid(std::move(cells), GridOpts{.min = min_width});
}

/// Cells stacked top to bottom, each stretched to the full width (flex
/// cross-stretch — the GTK "fill"). Pipe `| grow(1)` onto the child that
/// should take the leftover height.
[[nodiscard]] inline Element col(std::vector<Element> cells, int gap = 0)
{
    auto vb = detail::vstack();
    if (gap > 0) vb.gap(gap);
    return vb(std::move(cells));
}

} // namespace maya
