#pragma once
// maya::components::Markdown — Renders markdown text to Element tree
//
//   Markdown({.source = "# Hello\n**bold** and *italic*\n- item 1\n- item 2"})
//   Markdown({.source = response_text})
//
// Supports: headers (#-####), bold (**), italic (*), strikethrough (~~),
// inline code (`), code blocks (```), bullet lists (-/*/+), numbered lists,
// blockquotes (>), horizontal rules (---), links [text](url), diff blocks.
//
// Paragraphs: consecutive non-block lines are joined with spaces and
// word-wrapped across the available width via FlexWrap.

#include "core.hpp"
#include "divider.hpp"

namespace maya::components {

struct MarkdownProps {
    std::string source;
    Color       text_color    = palette().text;
    Color       heading_color = palette().primary;
    Color       code_bg       = palette().surface;
    Color       code_fg       = Color::rgb(180, 200, 220);
    Color       link_color    = palette().accent;
    Color       quote_color   = palette().muted;
    Color       bullet_color  = palette().muted;
};

namespace md_detail {

// ── Inline style parser ─────────────────────────────────────────────────────
// Handles **bold**, *italic*, ~~strikethrough~~, `code`, [link](url)

std::vector<Element> parse_inline(std::string_view line, const MarkdownProps& p);

// ── Word splitter ───────────────────────────────────────────────────────────
// Breaks styled text elements into per-word chunks so FlexWrap can
// wrap at word boundaries. Each word keeps its trailing whitespace.

std::vector<Element> wordify(std::vector<Element> parts);

// ── Block-line detection ────────────────────────────────────────────────────
// Returns true for lines that are block-level markdown constructs
// (headers, lists, quotes, fences, rules, blank lines).

bool is_block_line(std::string_view line);

} // namespace md_detail

// ── Markdown renderer ───────────────────────────────────────────────────────

Element Markdown(MarkdownProps props = {});

} // namespace maya::components
