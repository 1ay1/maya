#pragma once
// maya::widget::markdown — Terminal markdown rendering
//
// Parses a subset of Markdown and converts it to maya Element trees.
// Supports: headings, bold, italic, inline code, code blocks, lists,
// blockquotes, links, horizontal rules, and strikethrough.
//
// Usage:
//   auto ui = md("## Hello\nThis is **bold** and `code`.");

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

struct Text       { std::string content; };
struct Bold       { std::vector<struct Inline> children; };
struct Italic     { std::vector<struct Inline> children; };
struct Code       { std::string content; };
struct Link       { std::string text; std::string url; };
struct Strike     { std::vector<struct Inline> children; };

struct Inline {
    using Variant = std::variant<Text, Bold, Italic, Code, Link, Strike>;
    Variant inner;

    Inline(Text t)   : inner(std::move(t)) {}
    Inline(Bold b)   : inner(std::move(b)) {}
    Inline(Italic i) : inner(std::move(i)) {}
    Inline(Code c)   : inner(std::move(c)) {}
    Inline(Link l)   : inner(std::move(l)) {}
    Inline(Strike s) : inner(std::move(s)) {}
};

// ============================================================================
// Block AST nodes
// ============================================================================

struct Paragraph  { std::vector<Inline> spans; };
struct Heading    { int level; std::vector<Inline> spans; };
struct CodeBlock  { std::string content; std::string lang; };
struct Blockquote { std::vector<struct Block> children; };
struct ListItem   { std::vector<Inline> spans; };
struct List       { std::vector<ListItem> items; bool ordered; };
struct HRule      {};

struct Block {
    using Variant = std::variant<Paragraph, Heading, CodeBlock,
                                 Blockquote, List, HRule>;
    Variant inner;

    Block(Paragraph p)  : inner(std::move(p)) {}
    Block(Heading h)    : inner(std::move(h)) {}
    Block(CodeBlock c)  : inner(std::move(c)) {}
    Block(Blockquote b) : inner(std::move(b)) {}
    Block(List l)       : inner(std::move(l)) {}
    Block(HRule)        : inner(HRule{}) {}
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
// Only the trailing incomplete block is rendered as plain text.
// Each frame does O(new_chars) work, not O(total_chars).
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

    /// Build the element tree: cached blocks + raw tail.
    [[nodiscard]] Element build() const;

    /// Current full source text.
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
};

} // namespace maya
