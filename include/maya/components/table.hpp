#pragma once
// maya::components::Table — Grid with columns, headers, sorting, and scrolling
//
//   Table table({
//       .columns = {{"Name", 20}, {"Status", 12}, {"Count", 8, ColumnAlign::Right}},
//       .rows    = {{"server-1", "running", "42"}, {"server-2", "stopped", "0"}},
//   });
//
//   // In event handler:
//   table.update(ev);  // Up/Down/j/k, Left/Right sort col, Enter to sort
//
//   // In render:
//   table.render()

#include "core.hpp"

#include <algorithm>

namespace maya::components {

// ── Supporting types ────────────────────────────────────────────────────────

enum class ColumnAlign { Left, Center, Right };
enum class SortOrder   { None, Ascending, Descending };

struct Column {
    std::string header;
    int         width    = 0;         // 0 = auto (fit content)
    ColumnAlign align    = ColumnAlign::Left;
    bool        sortable = true;
};

struct TableProps {
    std::vector<Column>                   columns     = {};
    std::vector<std::vector<std::string>> rows        = {};
    int  max_visible  = 15;
    bool selectable   = true;
    bool show_header  = true;
    bool show_border  = true;
};

// ── Cell alignment helper ───────────────────────────────────────────────────

std::string align_cell(std::string_view content, int width, ColumnAlign align);

// ── Table component ─────────────────────────────────────────────────────────

class Table {
    int selected_  = 0;
    int scroll_    = 0;
    int sort_col_  = -1;
    SortOrder sort_order_ = SortOrder::None;

    TableProps props_;

    static constexpr int max_col_width = 40;

    void clamp();
    void ensure_visible();
    [[nodiscard]] std::vector<int> compute_widths() const;
    void toggle_sort();

public:
    explicit Table(TableProps props = {})
        : props_(std::move(props)) {
        if (!props_.columns.empty()) sort_col_ = 0;
    }

    // ── State access ─────────────────────────────────────────────────────────

    [[nodiscard]] int selected_row() const { return selected_; }
    void set_selected(int row) { selected_ = row; clamp(); }

    void set_rows(std::vector<std::vector<std::string>> rows);

    [[nodiscard]] int sort_column() const { return sort_col_; }
    [[nodiscard]] SortOrder sort_order() const { return sort_order_; }

    void sort_by(int col, SortOrder order);

    // ── Event handling ───────────────────────────────────────────────────────

    bool update(const Event& ev);

    // ── Render ───────────────────────────────────────────────────────────────

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
