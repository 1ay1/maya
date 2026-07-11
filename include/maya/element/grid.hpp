#pragma once
// maya::grid — the Bootstrap grid, re-thought for terminal cells.
//
// THE PROBLEM: a dashboard that must be correct at EVERY terminal size ends
// up as a hand-rolled tier switch — `if (w >= 200) wide(); else if (w >= 84)
// classic(); else narrow();` — with three separately-maintained layouts that
// drift apart the moment one is edited. Bootstrap solved this for the web 15
// years ago with ONE declaration: a 12-unit grid where each cell states its
// span PER BREAKPOINT and the framework re-solves the packing at every width.
//
// This is that, for cells instead of pixels:
//
//   grid({
//       col(cpu_panel),                    // span 12 (full width) by default
//       col(mem_panel).md(6).xl(3),        // half at md, quarter at xl
//       col(net_panel).md(6).xl(3),
//       col(sidebar).xl(Columns{40}),      // FIXED 40 cells at xl (TUI col-auto)
//       col(debug).span(0).lg(12),         // hidden until lg
//   })
//
// Mobile-first, like Bootstrap: a value set at a tier applies to every tier
// ABOVE it until overridden. Nothing set → span 12 → everything stacks in one
// column on a narrow terminal, automatically. The grid re-solves from the
// REAL width of the slot it sits in (adapt() under the hood), so a grid in a
// sidebar collapses independently of one in the main pane.
//
// Semantics:
//   * `columns` grid units per row (default 12). A cell's span is its
//     fraction of the row: span 6 of 12 = half the width.
//   * Cells pack into rows greedily, in order; a cell that would overflow
//     the row's units starts a new row.
//   * span 0 (or Columns{0}) = hidden at that tier and above (until reset).
//   * Columns{n} = a FIXED n-cell column (sidebars, gutters, rails). Fixed
//     cells consume no grid units — the row's span cells share what remains.
//   * Widths solve EXACTLY: when a row's spans sum to `columns`, the row
//     fills its slot to the last cell (largest-remainder distribution — no
//     ragged right edge from integer division). Spans that sum short leave
//     Bootstrap-style trailing space.
//   * .order(n): cells sort by order (stable) before packing — put the
//     process table first on a phone, third on an ultrawide.
//   * GridOpts{.grow_rows = true}: rows share surplus height equally when
//     the grid sits in a definite-height slot (full-screen dashboards).
//
// Breakpoints are SLOT-width cells, tuned for terminals (defaults):
//   xs < 60 ≤ sm < 90 ≤ md < 120 ≤ lg < 160 ≤ xl < 200 ≤ xxl
// Override per grid via GridOpts{.breaks = Breaks{...}}.

#include "builder.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace maya {

// ============================================================================
// Breakpoints
// ============================================================================

enum class Bk : std::uint8_t { XS = 0, SM, MD, LG, XL, XXL };

struct Breaks {
    int sm = 60, md = 90, lg = 120, xl = 160, xxl = 200;

    [[nodiscard]] constexpr Bk tier(int w) const noexcept {
        if (w >= xxl) return Bk::XXL;
        if (w >= xl)  return Bk::XL;
        if (w >= lg)  return Bk::LG;
        if (w >= md)  return Bk::MD;
        if (w >= sm)  return Bk::SM;
        return Bk::XS;
    }
};

// ============================================================================
// GridCol — one cell of the grid, with per-breakpoint width rules
// ============================================================================

struct GridCol {
    static constexpr int kUnset = -1;

    Element el;
    int span_[6]  = {kUnset, kUnset, kUnset, kUnset, kUnset, kUnset};
    int fixed_[6] = {kUnset, kUnset, kUnset, kUnset, kUnset, kUnset};
    int order_ = 0;

    explicit GridCol(Element e) : el(std::move(e)) {}

    // -- xs-and-up (mobile-first base) --------------------------------------
    GridCol& span(int units)   { return set_(Bk::XS, units); }
    GridCol& span(Columns c)   { return set_(Bk::XS, c); }

    // -- per-tier overrides (apply to the tier and everything above) --------
    GridCol& xs(int u)  { return set_(Bk::XS, u); }
    GridCol& sm(int u)  { return set_(Bk::SM, u); }
    GridCol& md(int u)  { return set_(Bk::MD, u); }
    GridCol& lg(int u)  { return set_(Bk::LG, u); }
    GridCol& xl(int u)  { return set_(Bk::XL, u); }
    GridCol& xxl(int u) { return set_(Bk::XXL, u); }

    GridCol& xs(Columns c)  { return set_(Bk::XS, c); }
    GridCol& sm(Columns c)  { return set_(Bk::SM, c); }
    GridCol& md(Columns c)  { return set_(Bk::MD, c); }
    GridCol& lg(Columns c)  { return set_(Bk::LG, c); }
    GridCol& xl(Columns c)  { return set_(Bk::XL, c); }
    GridCol& xxl(Columns c) { return set_(Bk::XXL, c); }

    /// Packing order (stable sort; lower first). Default 0 = declaration order.
    GridCol& order(int n) { order_ = n; return *this; }

    // -- resolution ----------------------------------------------------------
    struct Resolved {
        bool hidden   = false;
        bool is_fixed = false;   ///< amount is cells, not grid units
        int  amount   = 0;
    };

    /// Resolve this cell's rule at tier `t`: the nearest value set at or
    /// below `t` wins (mobile-first inheritance). Nothing set → full width.
    [[nodiscard]] Resolved at(Bk t, int default_span) const noexcept {
        for (int i = static_cast<int>(t); i >= 0; --i) {
            if (fixed_[i] != kUnset)
                return {fixed_[i] <= 0, true, fixed_[i]};
            if (span_[i] != kUnset)
                return {span_[i] <= 0, false, span_[i]};
        }
        return {false, false, default_span};
    }

private:
    GridCol& set_(Bk t, int units) {
        span_[static_cast<int>(t)]  = units;
        fixed_[static_cast<int>(t)] = kUnset;
        return *this;
    }
    GridCol& set_(Bk t, Columns c) {
        fixed_[static_cast<int>(t)] = c.value;
        span_[static_cast<int>(t)]  = kUnset;
        return *this;
    }
};

/// Bootstrap-flavored factory: col(element).md(6).xl(3)
[[nodiscard]] inline GridCol col(Element el) { return GridCol{std::move(el)}; }

// ============================================================================
// grid()
// ============================================================================

struct GridOpts {
    int    columns   = 12;      ///< grid units per row
    int    gap_x     = 1;       ///< cells between columns
    int    gap_y     = 0;       ///< rows between rows
    bool   grow_rows = false;   ///< rows share surplus height (definite slot)
    Breaks breaks{};            ///< tier thresholds (slot-width cells)
};

[[nodiscard]] inline auto grid(std::vector<GridCol> cols, GridOpts opts = {})
    -> ComponentBuilder
{
    return detail::adapt([cols = std::move(cols), opts](int w) -> Element {
        const Bk t = opts.breaks.tier(w);
        const int units_per_row = opts.columns > 0 ? opts.columns : 12;

        // 1. Resolve every cell at this tier; drop hidden ones.
        struct Item {
            const GridCol* c;
            bool fixed;
            int  amount;
            int  order;
        };
        std::vector<Item> items;
        items.reserve(cols.size());
        for (const auto& c : cols) {
            auto r = c.at(t, units_per_row);
            if (r.hidden) continue;
            int amt = r.amount;
            if (!r.is_fixed && amt > units_per_row) amt = units_per_row;
            items.push_back({&c, r.is_fixed, amt, c.order_});
        }
        if (items.empty()) return Element{ElementList{}};

        // 2. Order, then pack greedily into rows by grid units. Fixed cells
        //    consume no units (they take real width off the top instead).
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) {
                             return a.order < b.order;
                         });
        struct Row {
            std::vector<const Item*> cells;
            int units = 0;
        };
        std::vector<Row> rows;
        rows.emplace_back();
        for (const auto& it : items) {
            const int u = it.fixed ? 0 : it.amount;
            Row& cur = rows.back();
            if (!cur.cells.empty() && cur.units + u > units_per_row)
                rows.emplace_back();
            rows.back().cells.push_back(&it);
            rows.back().units += u;
        }

        // 3. Solve each row's widths exactly and build it.
        std::vector<Element> row_els;
        row_els.reserve(rows.size());
        for (const Row& row : rows) {
            const int n = static_cast<int>(row.cells.size());
            const int gaps = opts.gap_x * (n > 1 ? n - 1 : 0);
            int fixed_total = 0;
            for (const Item* it : row.cells)
                if (it->fixed) fixed_total += it->amount;
            const int avail = std::max(0, w - gaps - fixed_total);

            // floor(span/columns * avail) per span cell, then hand the
            // integer-division remainder out one cell at a time left→right
            // when the row is FULL (units == columns) so it fills exactly.
            std::vector<int> cw(static_cast<std::size_t>(n), 0);
            int span_used = 0;
            for (int i = 0; i < n; ++i) {
                const Item* it = row.cells[static_cast<std::size_t>(i)];
                if (it->fixed) {
                    cw[static_cast<std::size_t>(i)] = it->amount;
                } else {
                    int width_i = static_cast<int>(
                        static_cast<long long>(it->amount) * avail
                        / units_per_row);
                    if (width_i < 1 && avail > 0) width_i = 1;
                    cw[static_cast<std::size_t>(i)] = width_i;
                    span_used += width_i;
                }
            }
            if (row.units >= units_per_row) {
                int leftover = avail - span_used;
                for (int i = 0; i < n && leftover > 0; ++i) {
                    if (row.cells[static_cast<std::size_t>(i)]->fixed) continue;
                    cw[static_cast<std::size_t>(i)] += 1;
                    --leftover;
                }
            }

            std::vector<Element> cell_els;
            cell_els.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                // A Column-direction box with a fixed width: the default
                // cross-axis Stretch hands the cell content the FULL cell
                // width, and the row's own Stretch hands it the row height.
                auto cb = detail::vstack();
                cb.width(Dimension::fixed(cw[static_cast<std::size_t>(i)]));
                cell_els.push_back(
                    cb(Element{row.cells[static_cast<std::size_t>(i)]->c->el}));
            }
            auto rb = detail::hstack();
            if (opts.gap_x > 0) rb.gap(opts.gap_x);
            if (opts.grow_rows) rb.grow(1.0f);
            row_els.push_back(rb(std::move(cell_els)));
        }

        if (row_els.size() == 1) return std::move(row_els.front());
        auto vb = detail::vstack();
        if (opts.gap_y > 0) vb.gap(opts.gap_y);
        return vb(std::move(row_els));
    });
}

} // namespace maya
