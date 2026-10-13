#pragma once
// maya::layout::columns — a shared column solver for responsive tables.
//
// THE BUG CLASS THIS KILLS: a table renders its header row and its body rows
// from SEPARATE code paths, each with its own hand-written width breakpoints
// ("show the PORT column when w >= 92"). The moment one site is edited and
// the other isn't, the header drifts off the value rail — a whole family of
// ragged-table bugs that only shows up at specific terminal widths.
//
// THE FIX: describe every column ONCE as a ColSpec (minimum width, optional
// max, a flex weight for surplus space, and a `keep` rank for drop order),
// solve the set against the real available width ONCE per frame, and render
// header + every row from the same ColPlan. Desync becomes structurally
// impossible: there is one plan, and everyone reads it.
//
//   using namespace maya;
//   enum Col { PID, NAME, CPU, MEM, N };
//   std::array<ColSpec, N> spec{{
//       {.min = 8},                                 // PID   — fixed, essential
//       {.min = 8, .weight = 1},                    // NAME  — takes the slack
//       {.min = 6},                                 // CPU   — fixed, essential
//       {.min = 8, .keep = 1},                      // MEM   — first to drop
//   }};
//   ColPlan plan = solve_columns(spec, avail_w, /*gap=*/1);
//   // header AND rows:
//   if (plan.has(MEM)) cols.push_back(cell | width(plan.at(MEM)));
//
// Semantics:
//   * Columns whose minimums don't fit are dropped lowest-`keep` first
//     (ties drop the rightmost). keep == kKeepAlways never drops — if even
//     the essentials overflow, they are kept and the row overflows (flex
//     shrink / clipping downstream handles it).
//   * Surplus space is distributed to `weight > 0` columns in proportion
//     to weight, clamped at `max` (0 = unbounded). With at least one
//     unbounded weighted column the plan always fills `avail` exactly, so
//     right-aligned rails stay pinned.
//   * A dropped column solves to width 0 → `has()` is false; emit nothing
//     (including its gap) for it and every consumer stays aligned.
//
// Pure arithmetic — no Element types, no layout engine. Cheap enough to
// solve every frame.

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace maya {

/// `keep` rank meaning "never drop this column / item".
inline constexpr int kKeepAlways = std::numeric_limits<int>::max();

struct ColSpec {
    int   min    = 1;            ///< minimum content width (cells)
    int   max    = 0;            ///< growth cap; 0 = unbounded (weight > 0 only)
    float weight = 0.0f;         ///< share of surplus; 0 = fixed at min
    int   keep   = kKeepAlways;  ///< drop order: LOWER dropped first
};

struct ColPlan {
    std::vector<int> width;      ///< solved widths; 0 = dropped
    int gap = 0;                 ///< the gap the plan was solved with

    /// Is column i visible in this plan?
    [[nodiscard]] bool has(std::size_t i) const noexcept {
        return i < width.size() && width[i] > 0;
    }
    /// Solved width of column i (0 when dropped / out of range).
    [[nodiscard]] int at(std::size_t i) const noexcept {
        return i < width.size() ? width[i] : 0;
    }
    /// Total cells consumed: visible widths + gaps between visible columns.
    [[nodiscard]] int used() const noexcept {
        long long t = 0;
        int c = 0;
        for (int w : width)
            if (w > 0) { t += w; ++c; }
        if (c > 1) t += static_cast<long long>(gap) * (c - 1);
        return static_cast<int>(t);
    }
};

[[nodiscard]] inline ColPlan solve_columns(std::span<const ColSpec> cols,
                                           int avail, int gap = 1)
{
    const std::size_t n = cols.size();
    ColPlan plan;
    plan.gap = gap;
    plan.width.assign(n, 0);

    std::vector<char> vis(n, 1);

    // Cells the visible minimums need, including inter-column gaps. A
    // zero-min fixed column occupies no cells and no gap (it's inert until
    // it either has weight or a positive min).
    auto occupies = [&](std::size_t i) {
        return vis[i] && (cols[i].min > 0 || cols[i].weight > 0.0f);
    };
    auto need = [&]() -> long long {
        long long t = 0;
        int c = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (occupies(i)) { t += cols[i].min > 0 ? cols[i].min : 0; ++c; }
        if (c > 1) t += static_cast<long long>(gap) * (c - 1);
        return t;
    };

    // 1. Drop until the minimums fit: lowest `keep` first, ties → rightmost.
    //    kKeepAlways columns never drop; if only those remain the row simply
    //    overflows and downstream flex-shrink/clipping deals with it.
    while (need() > avail) {
        int best = -1;
        int best_keep = kKeepAlways;
        for (std::size_t i = 0; i < n; ++i) {
            if (!vis[i] || cols[i].keep >= kKeepAlways) continue;
            if (cols[i].keep <= best_keep) {
                best = static_cast<int>(i);
                best_keep = cols[i].keep;
            }
        }
        if (best < 0) break;
        vis[static_cast<std::size_t>(best)] = 0;
    }

    for (std::size_t i = 0; i < n; ++i)
        if (occupies(i) && cols[i].min > 0) plan.width[i] = cols[i].min;

    // 2. Distribute surplus to weighted visible columns, proportional to
    //    weight, clamped at max. Loop handles clamp cascades; the 1-cell
    //    round-robin pass mops up integer-rounding residue.
    long long surplus = avail - need();
    auto eligible = [&](std::size_t i) {
        return vis[i] && cols[i].weight > 0.0f &&
               (cols[i].max <= 0 || plan.width[i] < cols[i].max);
    };
    while (surplus > 0) {
        double tw = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            if (eligible(i)) tw += static_cast<double>(cols[i].weight);
        if (tw <= 0.0) break;

        const long long pass = surplus;
        bool gave = false;
        for (std::size_t i = 0; i < n && surplus > 0; ++i) {
            if (!eligible(i)) continue;
            auto share = static_cast<long long>(
                static_cast<double>(pass) *
                static_cast<double>(cols[i].weight) / tw);
            if (cols[i].max > 0)
                share = std::min<long long>(share, cols[i].max - plan.width[i]);
            share = std::min(share, surplus);
            if (share > 0) {
                plan.width[i] += static_cast<int>(share);
                surplus -= share;
                gave = true;
            }
        }
        if (!gave) {
            // Rounding tail: hand out one cell at a time.
            for (std::size_t i = 0; i < n && surplus > 0; ++i) {
                if (!eligible(i)) continue;
                plan.width[i] += 1;
                --surplus;
                gave = true;
            }
            if (!gave) break;
        }
    }

    return plan;
}

} // namespace maya
