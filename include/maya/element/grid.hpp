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
//   columns({sec1, sec2, sec3, …}, 34);
//
// The OTHER flow: newspaper columns. grid() wraps left-to-right (cell 2 sits
// beside cell 1); columns() runs top-to-bottom (cell 2 sits BELOW cell 1,
// and the sequence moves to column 2 only when column 1 has had its share of
// the height). Reading order is down-then-across, which is what a document
// wants and what a dashboard does not. One number again: how wide a column
// needs to be before splitting is worth it.
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
#include "../core/render_context.hpp"

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

// ============================================================================
// columns() — newspaper flow
// ============================================================================

struct ColumnsOpts {
    // The WIDEST a column may get before splitting again. Not a minimum:
    // the question a document asks is "how long may a line be before it is
    // tiring to read", and the answer forces the count — a 200-column slot
    // with max 60 gives three columns because two would be 99 wide.
    //
    // Stating the CEILING rather than the floor is what makes the result
    // exact. A minimum leaves a remainder nobody owns (fit as many as pay
    // for themselves, then live with the slack); a maximum picks the count
    // first and then divides the WHOLE slot among them, so every cell of
    // the width is claimed by construction.
    int max_width = 60;
    // Ceiling on the count, for slots wide enough to split further than the
    // content deserves. 0 = as many as the width implies.
    int max_cols = 0;
    // The narrowest a column may be. A CEILING alone cannot express "do not
    // split below what a row needs": at max_width 64 a 76-column slot splits
    // into two columns of 36, and a label─→value row that wanted 40 starts
    // truncating its labels. That trades one unreadable layout for another.
    //
    // So the two bounds do different jobs and both are needed. max_width is
    // about READING (how long a line may get); min_width is about FITTING
    // (how short a column may get before its content stops working). A split
    // happens only where both are satisfied; where they conflict, min wins,
    // because a too-wide line is awkward while a truncated one has lost
    // information.
    int min_width = 0;
    // Blank columns between neighbours. Two: one reads as a wrapped line
    // rather than a gutter once the cells have ragged right edges.
    int gap = 2;
    // Balance the column HEIGHTS rather than packing strictly in sequence.
    bool balance = true;
    // The widest slot the caller can actually be given. 0 = trust the
    // offered width completely.
    //
    // A BOUND, not an override, and the distinction is the whole point.
    // adapt() reports the slot a component is MEASURED in, which is not
    // always the slot it is PAINTED in: a sibling that flex has not
    // subtracted yet (a scrollbar gutter) is invisible at measure time, so
    // the offer can be a column or two generous. Laying out for it puts the
    // tail of every row — which is where a value sits — under whatever that
    // sibling draws.
    //
    // Taking the MINIMUM of the two is what makes it safe in both
    // directions: it can hand back an over-generous offer, but it can never
    // pin the body narrower than the width it really has. Replacing the
    // offer outright was tried and left a constant strip of dead space down
    // the right edge at every terminal size.
    //
    // It also covers the measure-outside-a-render-pass case for free: the
    // renderer's no-context fallback is just another over-generous offer.
    int width = 0;
};

/// Cells flowed top-to-bottom into columns, each no wider than `max_width`.
///
/// The sibling of grid(): grid wraps ACROSS (cell 2 beside cell 1), columns
/// runs DOWN (cell 2 below cell 1, moving right only when the column has had
/// its share of the height). Reading order is down-then-across — a document,
/// not a dashboard.
///
/// Two properties, both load-bearing:
///
///   * NO SLACK. The count is the smallest k whose columns fit under
///     max_width, and the slot is then divided among exactly those k with a
///     largest-remainder spread. The columns plus gaps sum to the slot
///     WIDTH, not to something near it — there is no strip left over,
///     because no step ever rounds a width down and keeps the change.
///
///   * RESPONSIVE INSIDE. Each column is a fixed-width box whose cross-axis
///     Stretch hands its content that full width, so a cell laid out with
///     adapt()/grid()/a meter re-solves against the COLUMN — not against
///     the screen. Nesting columns() inside a column therefore works, and a
///     figure sized to the screen inside a third of it is impossible.
///
/// Balanced by MEASURED HEIGHT, never by cell count: cells differ wildly in
/// length (a two-row table against a nine-bucket chart), and splitting on
/// count leaves one column twice the height of its neighbour, which reads as
/// a layout bug rather than as a choice.
///
/// Like grid(), this measures the REAL slot width via adapt() — so the count
/// is re-decided on every resize and no caller has to know what a frame, a
/// padding or a scrollbar gutter cost. A caller that computes its own column
/// width against a guessed "usable" figure is the bug this replaces: the
/// cell measures one width, flex resolves another, and the difference comes
/// off the end of the string.
[[nodiscard]] inline auto columns(std::vector<Element> cells, ColumnsOpts opts)
    -> ComponentBuilder
{
    return detail::adapt([cells = std::move(cells), opts](int offered) -> Element {
        // The offer bounds us and so does the caller's width; take the
        // smaller. Never lay out wider than the slot we will be painted in,
        // never narrower than the width we actually have.
        const int w = opts.width > 0 ? std::min(offered, opts.width) : offered;
        const int n = static_cast<int>(cells.size());
        if (n == 0) return Element{ElementList{}};

        const int gap  = std::max(0, opts.gap);
        const int most = std::max(1, opts.max_width);

        // The smallest k whose columns come in at or under max_width.
        //
        // k columns leave w - gap*(k-1) to divide, so the widest column is
        // ceil((w - gap*(k-1)) / k). Solving that for "<= most" directly is
        // fiddly and easy to get wrong by one; counting up from 1 is exact,
        // obviously right, and bounded by n.
        int cols = 1;
        while (cols < n) {
            const int span = w - gap * (cols - 1);
            if (span <= 0) break;
            if ((span + cols - 1) / cols <= most) break;   // ceil <= most
            // Would the NEXT column take us below the floor? Then stop here
            // — splitting further would satisfy the ceiling by breaking the
            // content, which is not an improvement.
            if (opts.min_width > 0) {
                const int next = w - gap * cols;
                if (next / (cols + 1) < opts.min_width) break;
            }
            ++cols;
        }
        cols = std::clamp(cols, 1, n);
        if (opts.max_cols > 0) cols = std::min(cols, opts.max_cols);

        // One column: hand back the plain stack, at the FULL slot width.
        //
        // Not an optimisation — it is the guarantee that enabling flow
        // cannot disturb a narrow layout, because at k == 1 there is no
        // wrapper here to disturb it.
        //
        // And no cap, ever. max_width's job is to decide HOW MANY columns
        // there are; it is not a margin. Once the answer is one there is no
        // second column for withheld width to go to, so capping leaves a
        // strip of the slot belonging to nobody — which is the exact thing
        // this primitive exists to prevent, just arriving from the other
        // side. An earlier version capped whenever the body could not split
        // (a single section, or max_cols == 1), and it cost a one-section
        // stats tab 19 columns at width 90: the row stopped at 65 with the
        // value stranded mid-row, AND the label truncated, because the lane
        // it was sharing had been shrunk to a width the slot never asked
        // for. A bound that costs width without buying a split is not a
        // ceiling.
        if (cols == 1) {
            auto vb = detail::vstack();
            // Claim the slot EXPLICITLY rather than trusting the stack to
            // stretch into it. Inside adapt() the component reports the size
            // it measures, and a stack of rows measures to its NATURAL width
            // — the longest row — not to the slot it was offered. The body
            // then reports 67 on an 83-column slot and the 16 columns nobody
            // claimed show up as dead space down the right edge, which is
            // the same "strip belonging to nobody" this primitive exists to
            // prevent, one level up from where it was fixed before.
            //
            // max_width bounds it as well as the offer does. A row whose
            // cells shrink under pressure (a label that truncates so a value
            // survives) only does so when the width it is given is the width
            // it actually has — handed the parent's larger slot instead, the
            // row sees no pressure, never truncates, and its tail runs off
            // the end. That is a VALUE silently disappearing, which is worse
            // than any truncated label.
            vb.width(Dimension::fixed(w));
            vb.max_width(Dimension::fixed(w));
            return vb(std::move(cells));
        }

        // Divide the slot EXACTLY: base + largest-remainder spread. The
        // first `rem` columns take one extra cell, so the widths sum to the
        // full span and the last column ends flush with the slot edge.
        const int span = std::max(cols, w - gap * (cols - 1));
        const int base = span / cols;
        const int rem  = span % cols;
        std::vector<int> cw(static_cast<std::size_t>(cols));
        for (int i = 0; i < cols; ++i)
            cw[static_cast<std::size_t>(i)] = base + (i < rem ? 1 : 0);

        // Measure every cell at the NARROWEST column. A cell's height must
        // not depend on which column it lands in, or the balance below would
        // be computed from heights the layout then contradicts.
        std::vector<int> h(static_cast<std::size_t>(n), 1);
        int total = 0;
        for (int i = 0; i < n; ++i) {
            const int m = measure_element(cells[static_cast<std::size_t>(i)],
                                          std::max(1, base)).height.value;
            h[static_cast<std::size_t>(i)] = std::max(1, m);
            total += h[static_cast<std::size_t>(i)];
        }

        // Fill column by column, re-deriving the target from what is LEFT.
        //
        // A fixed target of total/cols does not survive contact with atomic
        // cells: once one column overshoots (it must, unless the heights
        // divide evenly) every later column inherits the error, and the last
        // one collects it. Recomputing rem_total/rem_cols after each column
        // spreads that error instead of accumulating it.
        //
        // The `(n - i) > (rem_cols - 1)` guard reserves one cell for each
        // column still to come, so a tall early cell can never starve the
        // right-hand columns into being empty.
        std::vector<std::vector<Element>> col_cells(static_cast<std::size_t>(cols));
        int i = 0, rem_total = total;
        for (int c = 0; c < cols; ++c) {
            const int rem_cols = cols - c;
            const int target   = (rem_total + rem_cols - 1) / rem_cols;
            // The LAST column takes everything that is left, unconditionally.
            // Deriving its contents from the same target arithmetic would
            // make "every cell is placed" a property of the rounding rather
            // than of the loop, and a cell silently dropped by a layout is
            // the worst failure this file could have.
            const bool last = (c == cols - 1);
            int used = 0;
            while (i < n && (last || (n - i) > (rem_cols - 1))) {
                if (!last && opts.balance && used > 0 && used >= target) break;
                col_cells[static_cast<std::size_t>(c)]
                    .push_back(std::move(cells[static_cast<std::size_t>(i)]));
                used      += h[static_cast<std::size_t>(i)];
                rem_total -= h[static_cast<std::size_t>(i)];
                ++i;
            }
        }

        // A Column-direction box with a fixed width: the default cross-axis
        // Stretch hands the content the FULL column width, which is what
        // makes a cell inside re-solve against its column.
        std::vector<Element> col_els;
        col_els.reserve(static_cast<std::size_t>(cols));
        for (int i = 0; i < cols; ++i) {
            auto cb = detail::vstack();
            cb.width(Dimension::fixed(cw[static_cast<std::size_t>(i)]));
            col_els.push_back(cb(std::move(col_cells[static_cast<std::size_t>(i)])));
        }

        auto rb = detail::hstack();
        if (gap > 0) rb.gap(gap);
        return rb(std::move(col_els));
    });
}

/// Sugar: `columns(cells, 60)` — "no column wider than 60".
[[nodiscard]] inline auto columns(std::vector<Element> cells, int max_width)
    -> ComponentBuilder
{
    return columns(std::move(cells), ColumnsOpts{.max_width = max_width});
}

} // namespace maya

