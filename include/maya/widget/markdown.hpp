#pragma once
// maya::widget::markdown — Full-featured terminal markdown rendering
//
// Parses CommonMark + GFM markdown and converts to maya Element trees.
// Supports: headings (ATX + setext), bold, italic, bold+italic, inline code,
// fenced + indented code blocks, blockquotes, nested lists (ordered/unordered),
// task lists, tables, links, images, footnotes, strikethrough, horizontal rules,
// backslash escapes, autolinks, and hard line breaks.
//
// Usage:
//   auto ui = markdown("## Hello\nThis is **bold** and `code`.");

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../element/builder.hpp"

namespace maya {
namespace md {

// ============================================================================
// Inline AST nodes (within paragraphs/headings)
// ============================================================================

struct Text        { std::string content; };
struct Bold        { std::vector<struct Inline> children; };
struct Italic      { std::vector<struct Inline> children; };
struct BoldItalic  { std::vector<struct Inline> children; };
struct Code        { std::string content; };
struct Link        { std::string text; std::string url; };
struct Image       { std::string alt; std::string url; };
struct Strike      { std::vector<struct Inline> children; };
struct FootnoteRef { std::string label; };
struct HardBreak   {};

struct Inline {
    using Variant = std::variant<Text, Bold, Italic, BoldItalic, Code,
                                 Link, Image, Strike, FootnoteRef, HardBreak>;
    Variant inner;

    Inline(Text t)        : inner(std::move(t)) {}
    Inline(Bold b)        : inner(std::move(b)) {}
    Inline(Italic i)      : inner(std::move(i)) {}
    Inline(BoldItalic bi) : inner(std::move(bi)) {}
    Inline(Code c)        : inner(std::move(c)) {}
    Inline(Link l)        : inner(std::move(l)) {}
    Inline(Image im)      : inner(std::move(im)) {}
    Inline(Strike s)      : inner(std::move(s)) {}
    Inline(FootnoteRef f) : inner(std::move(f)) {}
    Inline(HardBreak)     : inner(HardBreak{}) {}
};

// ============================================================================
// Block AST nodes
// ============================================================================

struct Paragraph   { std::vector<Inline> spans; };
struct Heading     { int level; std::vector<Inline> spans; };
struct CodeBlock   { std::string content; std::string lang; };
struct Blockquote  { std::vector<struct Block> children; };

struct ListItem {
    std::vector<Inline> spans;                // first-line inline content
    std::vector<struct Block> children;       // sub-blocks (nested lists, multi-para)
    std::optional<bool> checked;              // nullopt = normal, true/false = task
};
struct List        { std::vector<ListItem> items; bool ordered; int start_num = 1; };

struct HRule       {};
struct TableCell   { std::vector<Inline> spans; };
struct TableRow    { std::vector<TableCell> cells; };
struct Table       { TableRow header; std::vector<TableRow> rows; };
struct FootnoteDef { std::string label; std::vector<struct Block> children; };

struct Block {
    using Variant = std::variant<Paragraph, Heading, CodeBlock,
                                 Blockquote, List, HRule, Table, FootnoteDef>;
    Variant inner;

    Block(Paragraph p)   : inner(std::move(p)) {}
    Block(Heading h)     : inner(std::move(h)) {}
    Block(CodeBlock c)   : inner(std::move(c)) {}
    Block(Blockquote b)  : inner(std::move(b)) {}
    Block(List l)        : inner(std::move(l)) {}
    Block(HRule)         : inner(HRule{}) {}
    Block(Table t)       : inner(std::move(t)) {}
    Block(FootnoteDef f) : inner(std::move(f)) {}
};

struct Document {
    std::vector<Block> blocks;
};

} // namespace md

// ============================================================================
// Parser
// ============================================================================

[[nodiscard]] md::Document parse_markdown(std::string_view source);

// ============================================================================
// AST to Element conversion
// ============================================================================

[[nodiscard]] Element md_inline_to_element(const md::Inline& span);
[[nodiscard]] Element md_block_to_element(const md::Block& block);

// ============================================================================
// Public API
// ============================================================================

/// Parse markdown and return an Element tree.
[[nodiscard]] Element markdown(std::string_view source);

// ============================================================================
// StreamingMarkdown — Progressive per-block rendering for streaming text
// ============================================================================
// Like Claude Code: completed blocks are parsed as markdown and cached.
// The trailing incomplete block is parsed each frame (not raw text).
// Each frame does O(new_chars) work for committed blocks, not O(total_chars).
//
// Usage:
//   StreamingMarkdown md;
//   md.append("# Hello\n\nSome **bold");   // "# Hello" → rendered as heading
//   md.append(" text**\n\nMore...");        // "Some **bold text**" → rendered
//   auto ui = md.build();
//   md.finish();  // finalize last block

class StreamingMarkdown {
    std::string source_;
    size_t committed_ = 0;              // bytes parsed into finalized blocks
    std::vector<Element> blocks_;       // cached rendered blocks
    bool in_code_fence_ = false;        // tracking ``` state

    // Find the end of the last complete block boundary.
    // Returns the byte offset up to which blocks are "complete".
    [[nodiscard]] size_t find_block_boundary() const noexcept;

public:
    StreamingMarkdown() = default;

    /// Replace the full content (for compatibility with streaming that
    /// replaces the entire string each frame, like `msg.content = ...`).
    void set_content(std::string_view content);

    /// Append new text (for true incremental streaming).
    void append(std::string_view text);

    /// Finalize: parse any remaining tail as markdown (call when stream ends).
    void finish();

    /// Reset all state for a new stream.
    void clear();

    /// Build the element tree: cached blocks + parsed tail.
    [[nodiscard]] Element build() const;

    /// Current full source text.
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
};

} // namespace maya
