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

} // namespace maya
