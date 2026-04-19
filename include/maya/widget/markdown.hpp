#pragma once
// maya::widget::markdown — Full-featured terminal markdown rendering
//
// Parses CommonMark + GFM markdown and converts to maya Element trees.
// Supports: headings (ATX + setext), bold, italic, bold+italic, inline code,
// fenced + indented code blocks, blockquotes (incl. GitHub alerts), nested
// lists (ordered/unordered), task lists, tables, links (inline, reference,
// collapsed, shortcut), images (incl. reference), footnotes (multi-paragraph,
// code), strikethrough, highlight (== ==), subscript (~x~) / superscript
// (^x^), horizontal rules, backslash escapes, autolinks, bare URLs, email
// autolinks, hard line breaks, HTML entities, emoji shortcodes, definition
// lists, an HTML subset (<br>, <kbd>, <mark>, <sub>, <sup>, <abbr>,
// <strong>/<em>/<span>, <details>/<summary>, <a id="…">), and GitHub-style
// @user / #123 / org/repo#42 references.
//
// Usage:
//   auto ui = markdown("## Hello\nThis is **bold** and `code`.");

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
struct Link        { std::string text; std::string url; std::string title; };
struct Image       { std::string alt;  std::string url; std::string title; };
struct Strike      { std::vector<struct Inline> children; };
struct Highlight   { std::vector<struct Inline> children; };    // ==text== or <mark>
struct Sub         { std::vector<struct Inline> children; };    // ~x~ or <sub>
struct Sup         { std::vector<struct Inline> children; };    // ^x^ or <sup>
struct Kbd         { std::vector<struct Inline> children; };    // <kbd>
struct Abbr        { std::string title; std::vector<struct Inline> children; }; // <abbr title="…">
struct Mention     {
    enum class Kind : uint8_t { User, Issue, CrossRepo };
    Kind        kind;
    std::string display;   // "@alice" / "#42" / "foo/bar#7"
    std::string url;       // best-effort GitHub URL
};
struct FootnoteRef { std::string label; };
struct HardBreak   {};

struct Inline {
    using Variant = std::variant<Text, Bold, Italic, BoldItalic, Code,
                                 Link, Image, Strike, Highlight, Sub, Sup,
                                 Kbd, Abbr, Mention, FootnoteRef, HardBreak>;
    Variant inner;

    Inline(Text t)        : inner(std::move(t)) {}
    Inline(Bold b)        : inner(std::move(b)) {}
    Inline(Italic i)      : inner(std::move(i)) {}
    Inline(BoldItalic bi) : inner(std::move(bi)) {}
    Inline(Code c)        : inner(std::move(c)) {}
    Inline(Link l)        : inner(std::move(l)) {}
    Inline(Image im)      : inner(std::move(im)) {}
    Inline(Strike s)      : inner(std::move(s)) {}
    Inline(Highlight h)   : inner(std::move(h)) {}
    Inline(Sub sb)        : inner(std::move(sb)) {}
    Inline(Sup sp)        : inner(std::move(sp)) {}
    Inline(Kbd k)         : inner(std::move(k)) {}
    Inline(Abbr a)        : inner(std::move(a)) {}
    Inline(Mention m)     : inner(std::move(m)) {}
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

// GitHub alert block: `> [!NOTE] ...`
struct Alert {
    enum class Kind : uint8_t { Note, Tip, Important, Warning, Caution };
    Kind kind = Kind::Note;
    std::vector<struct Block> children;
};

// Definition list: term  \n  : def \n  : another def
struct DefItem { std::vector<Inline> term; std::vector<std::vector<struct Block>> defs; };
struct DefList { std::vector<DefItem> items; };

// Collapsible <details>/<summary> section (rendered as a titled blockquote)
struct Details {
    std::vector<Inline>       summary;
    std::vector<struct Block> body;
};

// Arbitrary raw-HTML block — rendered as plain monospaced text.
struct HtmlBlock { std::string content; };

struct Block {
    using Variant = std::variant<Paragraph, Heading, CodeBlock,
                                 Blockquote, List, HRule, Table,
                                 FootnoteDef, Alert, DefList, Details, HtmlBlock>;
    Variant inner;

    Block(Paragraph p)   : inner(std::move(p)) {}
    Block(Heading h)     : inner(std::move(h)) {}
    Block(CodeBlock c)   : inner(std::move(c)) {}
    Block(Blockquote b)  : inner(std::move(b)) {}
    Block(List l)        : inner(std::move(l)) {}
    Block(HRule)         : inner(HRule{}) {}
    Block(Table t)       : inner(std::move(t)) {}
    Block(FootnoteDef f) : inner(std::move(f)) {}
    Block(Alert a)       : inner(std::move(a)) {}
    Block(DefList d)     : inner(std::move(d)) {}
    Block(Details d)     : inner(std::move(d)) {}
    Block(HtmlBlock h)   : inner(std::move(h)) {}
};

// Reference-link definition: `[label]: url "title"`.  Keys are lowercased.
struct LinkRef { std::string url; std::string title; };

struct Document {
    std::vector<Block>                              blocks;
    std::unordered_map<std::string, LinkRef>        ref_defs;
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

    // Ref-defs accumulated across every committed parse.  The tail parse
    // reuses this map so links pointing at earlier-committed `[label]: url`
    // still resolve, and so we avoid re-running collect_ref_defs() on the
    // tail every frame.
    std::unordered_map<std::string, md::LinkRef> ref_defs_;

    // ── per-frame cache ────────────────────────────────────────────────
    // build() is called every frame by the view layer. When neither source_
    // nor committed_ has moved since the last build, we return the cached
    // Element directly — no parse, no assembly.  Any mutator (append /
    // set_content / finish / clear) bumps `build_dirty_`.
    mutable Element cached_build_;
    mutable bool    build_dirty_  = true;
    mutable size_t  cached_tail_size_ = 0;   // tail length when cache was built

    // Find the end of the last complete block boundary.
    // Returns the byte offset up to which blocks are "complete".
    [[nodiscard]] size_t find_block_boundary() const noexcept;

    // Parse [committed_, boundary) — stash its ref defs, render its blocks.
    void commit_range(size_t boundary);

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

    /// Build the element tree: cached blocks + parsed tail. Returns a
    /// reference into the per-frame cache; the reference is valid until the
    /// next mutator call (append/set_content/finish/clear).
    [[nodiscard]] const Element& build() const;

    /// Current full source text.
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
};

} // namespace maya
