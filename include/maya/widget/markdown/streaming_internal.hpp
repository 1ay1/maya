// streaming_internal.hpp — declarations shared across the StreamingMarkdown TUs.
//
// streaming.cpp was carved into focused TUs (memo / boundary / commit /
// render_tail / build / folding / async). The helpers that were file-scope
// anonymous-namespace functions in the monolith — but are now called from
// more than one of those TUs — live here in the internal
// `maya::md_detail::streaming` namespace. Private to the implementation;
// NOT installed, NOT included by public consumers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "maya/element/element.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/ast.hpp"

namespace maya {

// assemble_markdown: Document → Element (vstack of block elements with the
// standard 2-col indent). Defined in markdown_memo.cpp; used there by the
// memoised markdown() entry point and re-exported via md_detail.
[[nodiscard]] Element assemble_markdown(md::Document&& doc);

namespace md_detail {
namespace streaming {

// ── FNV-1a 64-bit ─────────────────────────────────────────────────────────
// Branch-free, memory-bound content hash. Used by the markdown() LRU memo
// and by StreamingMarkdown's per-frame tail/prefix equality short-circuits.
[[nodiscard]] inline std::uint64_t fnv1a64(const char* data,
                                           std::size_t n) noexcept {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x100000001b3ULL;
    std::uint64_t h = kOffset;
    for (std::size_t k = 0; k < n; ++k) {
        h ^= static_cast<unsigned char>(data[k]);
        h *= kPrime;
    }
    return h;
}

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view s) noexcept {
    return fnv1a64(s.data(), s.size());
}

// ── Intra-list blank-line classifier ──────────────────────────────────────
// Classify a blank-line position `i` (src[i] == '\n' at line start) as a
// real block boundary (No), intra-list whitespace that must NOT split the
// list (Yes), or "next line not here yet, defer" (Unknown). Defined in
// boundary.cpp; the only caller is find_block_boundary in the same TU, but
// it is published here so the predicate has a single authoritative home.
enum class IntraBlank : std::uint8_t { No, Yes, Unknown };
[[nodiscard]] IntraBlank classify_blank_line(std::string_view src,
                                             std::size_t i) noexcept;

// ── GFM table finality predicate ───────────────────────────────────────────
// NotATable: the `|` is just literal prose. Incomplete: looks like a table
// but finality unproven (caller must not advance past this line).
// EndsAt: table proven complete; .pos is the byte index of the first
// non-`|` line after it. Defined in boundary.cpp.
enum class TableScan : std::uint8_t { NotATable, Incomplete, EndsAt };
struct TableScanResult {
    TableScan   kind;
    std::size_t pos;  // only meaningful for EndsAt
};
[[nodiscard]] TableScanResult find_table_end(std::string_view src,
                                             std::size_t line_start) noexcept;

} // namespace streaming
} // namespace md_detail
} // namespace maya
