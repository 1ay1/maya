#include "maya/components/table.hpp"

namespace maya::components {

std::string align_cell(std::string_view content, int width, ColumnAlign align) {
    int len = static_cast<int>(content.size());
    if (len >= width) return std::string(content.substr(0, width));
    int pad = width - len;
    switch (align) {
        case ColumnAlign::Left:
            return std::string(content) + std::string(pad, ' ');
        case ColumnAlign::Right:
            return std::string(pad, ' ') + std::string(content);
        case ColumnAlign::Center: {
            int left = pad / 2;
            return std::string(left, ' ') + std::string(content) + std::string(pad - left, ' ');
        }
    }
    return std::string(content);
}

void Table::clamp() {
    int n = static_cast<int>(props_.rows.size());
    if (n == 0) { selected_ = 0; scroll_ = 0; return; }
    if (selected_ < 0) selected_ = 0;
    if (selected_ >= n) selected_ = n - 1;
    ensure_visible();
}

void Table::ensure_visible() {
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + props_.max_visible)
        scroll_ = selected_ - props_.max_visible + 1;
    if (scroll_ < 0) scroll_ = 0;
}

std::vector<int> Table::compute_widths() const {
    int ncols = static_cast<int>(props_.columns.size());
    std::vector<int> widths(ncols, 0);

    for (int c = 0; c < ncols; ++c) {
        if (props_.columns[c].width > 0) {
            widths[c] = props_.columns[c].width;
            continue;
        }
        // Auto: scan header + all rows for max width.
        int w = static_cast<int>(props_.columns[c].header.size());
        // Account for sort indicator if this column is sortable.
        if (props_.columns[c].sortable) w += 2;  // " ▲" display width
        for (auto& row : props_.rows) {
            if (c < static_cast<int>(row.size()))
                w = std::max(w, static_cast<int>(row[c].size()));
        }
        widths[c] = std::min(w, max_col_width);
    }

    return widths;
}

void Table::toggle_sort() {
    if (sort_col_ < 0 || sort_col_ >= static_cast<int>(props_.columns.size()))
        return;
    if (!props_.columns[sort_col_].sortable) return;

    switch (sort_order_) {
        case SortOrder::None:       sort_by(sort_col_, SortOrder::Ascending);  break;
        case SortOrder::Ascending:  sort_by(sort_col_, SortOrder::Descending); break;
        case SortOrder::Descending: sort_by(sort_col_, SortOrder::None);       break;
    }
}

void Table::set_rows(std::vector<std::vector<std::string>> rows) {
    props_.rows = std::move(rows);
    clamp();
}

void Table::sort_by(int col, SortOrder order) {
    if (col < 0 || col >= static_cast<int>(props_.columns.size())) return;
    sort_col_   = col;
    sort_order_ = order;

    if (order == SortOrder::None) return;

    std::sort(props_.rows.begin(), props_.rows.end(),
        [col, order](const std::vector<std::string>& a,
                     const std::vector<std::string>& b) {
            const std::string& va = col < static_cast<int>(a.size()) ? a[col] : "";
            const std::string& vb = col < static_cast<int>(b.size()) ? b[col] : "";
            return order == SortOrder::Ascending ? va < vb : va > vb;
        });
}

bool Table::update(const Event& ev) {
    if (props_.rows.empty() && props_.columns.empty()) return false;

    // Row navigation
    if (props_.selectable && !props_.rows.empty()) {
        if (key(ev, SpecialKey::Up) || key(ev, 'k')) {
            --selected_; clamp(); return true;
        }
        if (key(ev, SpecialKey::Down) || key(ev, 'j')) {
            ++selected_; clamp(); return true;
        }
        if (key(ev, SpecialKey::PageUp)) {
            selected_ -= props_.max_visible; clamp(); return true;
        }
        if (key(ev, SpecialKey::PageDown)) {
            selected_ += props_.max_visible; clamp(); return true;
        }
        if (key(ev, SpecialKey::Home)) {
            selected_ = 0; clamp(); return true;
        }
        if (key(ev, SpecialKey::End)) {
            selected_ = static_cast<int>(props_.rows.size()) - 1; clamp(); return true;
        }
    }

    // Sort column navigation
    if (key(ev, SpecialKey::Left)) {
        if (sort_col_ > 0) { --sort_col_; return true; }
    }
    if (key(ev, SpecialKey::Right)) {
        int ncols = static_cast<int>(props_.columns.size());
        if (sort_col_ < ncols - 1) { ++sort_col_; return true; }
    }

    // Toggle sort on active column
    if (key(ev, SpecialKey::Enter) || key(ev, ' ')) {
        toggle_sort(); return true;
    }

    return false;
}

Element Table::render() const {
    using namespace maya::dsl;

    auto& p = palette();
    auto widths = compute_widths();
    int ncols = static_cast<int>(props_.columns.size());
    std::vector<Element> rows;

    auto build_row = [&](auto cell_fn, Style row_style) -> Element {
        std::string line;
        for (int c = 0; c < ncols; ++c) {
            if (c > 0 && props_.show_border) line += "│";
            else if (c > 0) line += " ";
            line += cell_fn(c);
        }
        return text(std::move(line), row_style, TextWrap::NoWrap);
    };

    // Header row
    if (props_.show_header && ncols > 0) {
        rows.push_back(build_row(
            [&](int c) -> std::string {
                std::string hdr = props_.columns[c].header;
                // Add sort indicator
                if (c == sort_col_ && sort_order_ != SortOrder::None) {
                    hdr += (sort_order_ == SortOrder::Ascending) ? " \xe2\x96\xb2" : " \xe2\x96\xbc";
                }
                return align_cell(hdr, widths[c], props_.columns[c].align);
            },
            Style{}.with_bold().with_fg(p.text)
        ));

        // Separator line
        std::string sep;
        for (int c = 0; c < ncols; ++c) {
            if (c > 0 && props_.show_border) sep += "\xe2\x94\xac";  // ┬
            else if (c > 0) sep += "\xe2\x94\x80";  // ─
            for (int i = 0; i < widths[c]; ++i)
                sep += "\xe2\x94\x80";  // ─
        }
        rows.push_back(text(std::move(sep), Style{}.with_fg(p.border), TextWrap::NoWrap));
    }

    // Data rows
    int n = static_cast<int>(props_.rows.size());
    if (n == 0) {
        rows.push_back(text("  (no data)", Style{}.with_fg(p.dim).with_italic()));
        return vstack()(std::move(rows));
    }

    int vis_end = std::min(n, scroll_ + props_.max_visible);
    for (int r = scroll_; r < vis_end; ++r) {
        bool is_selected = props_.selectable && r == selected_;
        Style row_style;
        if (is_selected) {
            row_style = Style{}.with_bold().with_fg(p.primary).with_bg(Color::rgb(30, 30, 45));
        } else {
            row_style = Style{}.with_fg(p.text);
        }

        auto& data_row = props_.rows[r];
        rows.push_back(build_row(
            [&](int c) -> std::string {
                std::string_view cell = (c < static_cast<int>(data_row.size()))
                    ? std::string_view(data_row[c])
                    : std::string_view("");
                return align_cell(cell, widths[c], props_.columns[c].align);
            },
            row_style
        ));
    }

    // Scroll indicator
    if (n > props_.max_visible) {
        std::string indicator;
        if (scroll_ > 0) indicator += "\xe2\x86\x91 ";  // ↑
        indicator += std::to_string(selected_ + 1) + "/" + std::to_string(n);
        if (vis_end < n) indicator += " \xe2\x86\x93";   // ↓
        rows.push_back(text(std::move(indicator), Style{}.with_fg(p.dim)));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
