// tables.cpp — GFM table line classification + cell splitting.
//
// Carved out of parser.cpp. These are pure line-level predicates/splitters
// the block parser calls when it sees a `| … |` row followed by a
// `|:--|--:|` delimiter row. No interaction with inline parsing or block
// state; the only shared utility is a leading/trailing whitespace trim,
// kept local here so the TU is self-contained.

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "maya/widget/markdown/ast.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {
namespace md_detail {

namespace {
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}
} // anonymous namespace

bool is_table_row(std::string_view line) {
    auto t = trim(line);
    if (t.empty() || t[0] != '|') return false;
    return t.find('|', 1) != std::string_view::npos;
}

bool is_table_separator(std::string_view line) {
    auto t = trim(line);
    if (t.empty() || t[0] != '|') return false;
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    }
    return t.find('-') != std::string_view::npos;
}

std::vector<std::string_view> split_table_cells(std::string_view line) {
    auto t = trim(line);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);

    std::vector<std::string_view> cells;
    size_t pos = 0;
    while (pos < t.size()) {
        // Handle escaped pipes within cells
        size_t pipe = pos;
        while (pipe < t.size()) {
            if (t[pipe] == '\\' && pipe + 1 < t.size()) { pipe += 2; continue; }
            if (t[pipe] == '|') break;
            ++pipe;
        }
        cells.push_back(trim(t.substr(pos, pipe - pos)));
        pos = (pipe < t.size()) ? pipe + 1 : t.size();
    }
    return cells;
}

// Parse the GFM delimiter row (`|:--|:-:|--:|`) into a per-column alignment
// list. Cell starts with `:` → left-anchored; ends with `:` → right-anchored;
// both → Center; neither → Left. Caller has verified is_table_separator(line).
std::vector<md::TableAlign> parse_table_alignments(std::string_view line) {
    std::vector<md::TableAlign> out;
    auto cells = split_table_cells(line);
    out.reserve(cells.size());
    for (auto& c : cells) {
        auto t = trim(c);
        bool left  = !t.empty() && t.front() == ':';
        bool right = !t.empty() && t.back()  == ':';
        if (left && right)      out.push_back(md::TableAlign::Center);
        else if (right)         out.push_back(md::TableAlign::Right);
        else                    out.push_back(md::TableAlign::Left);
    }
    return out;
}

} // namespace md_detail
} // namespace maya
