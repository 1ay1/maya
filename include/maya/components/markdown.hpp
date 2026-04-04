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

inline std::vector<Element> parse_inline(std::string_view line, const MarkdownProps& p) {
    using namespace maya::dsl;
    std::vector<Element> parts;
    std::string buf;

    auto flush = [&]() {
        if (!buf.empty()) {
            parts.push_back(text(std::move(buf), Style{}.with_fg(p.text_color)));
            buf.clear();
        }
    };

    size_t i = 0;
    while (i < line.size()) {
        // Strikethrough: ~~text~~
        if (i + 1 < line.size() && line[i] == '~' && line[i + 1] == '~') {
            flush();
            i += 2;
            size_t end = line.find("~~", i);
            if (end == std::string_view::npos) { buf += "~~"; continue; }
            parts.push_back(text(std::string(line.substr(i, end - i)),
                                 Style{}.with_strikethrough().with_fg(p.quote_color)));
            i = end + 2;
            continue;
        }

        // Bold: **text**
        if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
            flush();
            i += 2;
            size_t end = line.find("**", i);
            if (end == std::string_view::npos) { buf += "**"; continue; }
            parts.push_back(text(std::string(line.substr(i, end - i)),
                                 Style{}.with_bold().with_fg(p.text_color)));
            i = end + 2;
            continue;
        }

        // Italic: *text*
        if (line[i] == '*' && (i + 1 < line.size()) && line[i + 1] != '*') {
            flush();
            i += 1;
            size_t end = line.find('*', i);
            if (end == std::string_view::npos) { buf += '*'; continue; }
            parts.push_back(text(std::string(line.substr(i, end - i)),
                                 Style{}.with_italic().with_fg(p.text_color)));
            i = end + 1;
            continue;
        }

        // Inline code: `text`
        if (line[i] == '`') {
            flush();
            i += 1;
            size_t end = line.find('`', i);
            if (end == std::string_view::npos) { buf += '`'; continue; }
            parts.push_back(text(" " + std::string(line.substr(i, end - i)) + " ",
                                 Style{}.with_fg(p.code_fg).with_bg(p.code_bg)));
            i = end + 1;
            continue;
        }

        // Link: [text](url)
        if (line[i] == '[') {
            size_t close = line.find(']', i);
            if (close != std::string_view::npos && close + 1 < line.size() && line[close + 1] == '(') {
                size_t url_end = line.find(')', close + 2);
                if (url_end != std::string_view::npos) {
                    flush();
                    auto link_text = line.substr(i + 1, close - i - 1);
                    parts.push_back(text(std::string(link_text),
                                         Style{}.with_underline().with_fg(p.link_color)));
                    i = url_end + 1;
                    continue;
                }
            }
        }

        buf += line[i];
        ++i;
    }

    flush();
    return parts;
}

// ── Word splitter ───────────────────────────────────────────────────────────
// Breaks styled text elements into per-word chunks so FlexWrap can
// wrap at word boundaries. Each word keeps its trailing whitespace.

inline std::vector<Element> wordify(std::vector<Element> parts) {
    using namespace maya::dsl;
    std::vector<Element> words;

    for (auto& elem : parts) {
        auto* te = as_text(elem);
        if (!te || te->content.empty()) {
            words.push_back(std::move(elem));
            continue;
        }

        std::string_view sv = te->content;
        Style style = te->style;

        size_t pos = 0;
        while (pos < sv.size()) {
            size_t start = pos;
            // Consume non-space chars
            while (pos < sv.size() && sv[pos] != ' ') ++pos;
            // Attach trailing spaces to the word
            while (pos < sv.size() && sv[pos] == ' ') ++pos;
            words.push_back(text(std::string(sv.substr(start, pos - start)), style));
        }
    }

    return words;
}

// ── Block-line detection ────────────────────────────────────────────────────
// Returns true for lines that are block-level markdown constructs
// (headers, lists, quotes, fences, rules, blank lines).

inline bool is_block_line(std::string_view line) {
    if (line.empty()) return true;
    if (line.starts_with("```")) return true;
    if (line.starts_with("# ") || line.starts_with("## ") ||
        line.starts_with("### ") || line.starts_with("#### ")) return true;
    if (line.starts_with("> ") || line == ">") return true;

    // Bullet list: -, *, + followed by space
    if (line.size() >= 2 &&
        (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
        line[1] == ' ') return true;

    // Numbered list: digits followed by ". "
    if (line.size() >= 3) {
        size_t dot = line.find(". ");
        if (dot != std::string_view::npos && dot <= 3) {
            bool all_digits = true;
            for (size_t j = 0; j < dot; ++j)
                if (line[j] < '0' || line[j] > '9') { all_digits = false; break; }
            if (all_digits) return true;
        }
    }

    // Horizontal rule: 3+ identical chars from {-, *, _} optionally with spaces
    if (line.size() >= 3 &&
        (line.starts_with("---") || line.starts_with("***") || line.starts_with("___"))) {
        bool is_rule = true;
        char c = line[0];
        for (char ch : line) if (ch != c && ch != ' ') { is_rule = false; break; }
        if (is_rule) return true;
    }

    return false;
}

} // namespace md_detail

// ── Markdown renderer ───────────────────────────────────────────────────────

inline Element Markdown(MarkdownProps props) {
    using namespace maya::dsl;

    // Split source into lines
    std::vector<std::string_view> src_lines;
    {
        std::string_view src = props.source;
        while (!src.empty()) {
            auto nl = src.find('\n');
            if (nl == std::string_view::npos) {
                src_lines.push_back(src);
                break;
            }
            src_lines.push_back(src.substr(0, nl));
            src = src.substr(nl + 1);
        }
    }

    std::vector<Element> rows;
    bool in_code_block = false;
    std::string code_lang;
    std::vector<Element> code_lines;

    // Render inline content: parse styles, split words, wrap
    auto render_inline = [&](std::string_view content) -> Element {
        auto parts = md_detail::parse_inline(content, props);
        if (parts.size() == 1) {
            return std::move(parts[0]); // single text element wraps naturally
        }
        auto words = md_detail::wordify(std::move(parts));
        return hstack().wrap(FlexWrap::Wrap)(std::move(words));
    };

    // Render inline content for list items (prefixed with bullet/number)
    auto render_list_inline = [&](Element prefix, std::string_view content) -> Element {
        auto parts = md_detail::parse_inline(content, props);
        std::vector<Element> row;
        row.push_back(std::move(prefix));
        if (parts.size() == 1) {
            row.push_back(std::move(parts[0]));
            return hstack()(std::move(row));
        }
        auto words = md_detail::wordify(std::move(parts));
        for (auto& w : words) row.push_back(std::move(w));
        return hstack().wrap(FlexWrap::Wrap)(std::move(row));
    };

    // Process a block-level line
    auto process_block = [&](std::string_view line) {
        // Code block fence
        if (line.starts_with("```")) {
            if (!in_code_block) {
                in_code_block = true;
                code_lang = std::string(line.substr(3));
                code_lines.clear();
            } else {
                in_code_block = false;
                auto block = vstack()
                    .bg(props.code_bg)
                    .padding(0, 1, 0, 1);

                if (!code_lang.empty()) {
                    code_lines.insert(code_lines.begin(),
                        text(code_lang, Style{}.with_dim().with_fg(props.quote_color)));
                }

                rows.push_back(block(std::move(code_lines)));
            }
            return;
        }

        // Empty line
        if (line.empty()) {
            rows.push_back(text(""));
            return;
        }

        // Horizontal rule
        if (line.size() >= 3 &&
            (line.starts_with("---") || line.starts_with("***") || line.starts_with("___"))) {
            bool is_rule = true;
            char c = line[0];
            for (char ch : line) if (ch != c && ch != ' ') { is_rule = false; break; }
            if (is_rule) {
                rows.push_back(Divider({.color = props.quote_color}));
                return;
            }
        }

        // Headers
        if (line.starts_with("#### ")) {
            rows.push_back(text(std::string(line.substr(5)),
                                Style{}.with_bold().with_fg(props.heading_color)));
            return;
        }
        if (line.starts_with("### ")) {
            rows.push_back(text(std::string(line.substr(4)),
                                Style{}.with_bold().with_fg(props.heading_color)));
            return;
        }
        if (line.starts_with("## ")) {
            rows.push_back(text(std::string(line.substr(3)),
                                Style{}.with_bold().with_underline().with_fg(props.heading_color)));
            return;
        }
        if (line.starts_with("# ")) {
            rows.push_back(text(std::string(line.substr(2)),
                                Style{}.with_bold().with_underline().with_fg(props.heading_color)));
            return;
        }

        // Blockquote
        if (line.starts_with("> ") || line == ">") {
            auto content = line.size() > 2 ? line.substr(2) : std::string_view{""};
            auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
            rows.push_back(
                vstack()
                    .border(BorderStyle::Round)
                    .border_color(props.quote_color)
                    .border_sides(sides)
                    .padding(0, 1, 0, 0)(
                    text(std::string(content),
                         Style{}.with_italic().with_fg(props.quote_color))
                )
            );
            return;
        }

        // Bullet list (-, *, +)
        if (line.size() >= 2 &&
            (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            rows.push_back(render_list_inline(
                text("  • ", Style{}.with_fg(props.bullet_color)),
                line.substr(2)));
            return;
        }

        // Numbered list
        if (line.size() >= 3) {
            size_t dot = line.find(". ");
            if (dot != std::string_view::npos && dot <= 3) {
                bool all_digits = true;
                for (size_t j = 0; j < dot; ++j)
                    if (line[j] < '0' || line[j] > '9') { all_digits = false; break; }
                if (all_digits) {
                    auto num = line.substr(0, dot + 1);
                    rows.push_back(render_list_inline(
                        text("  " + std::string(num) + " ",
                             Style{}.with_fg(props.bullet_color)),
                        line.substr(dot + 2)));
                    return;
                }
            }
        }

        // Fallback: treat as paragraph line (shouldn't reach here normally)
        rows.push_back(render_inline(line));
    };

    // ── Main loop: join paragraphs, dispatch blocks ─────────────────────────

    size_t i = 0;
    while (i < src_lines.size()) {
        auto line = src_lines[i];

        // Inside code block — pass through until closing fence
        if (in_code_block) {
            if (line.starts_with("```")) {
                process_block(line); // ends code block
            } else {
                if (code_lang == "diff") {
                    Style s;
                    if (!line.empty() && line[0] == '+')
                        s = Style{}.with_fg(palette().diff_add);
                    else if (!line.empty() && line[0] == '-')
                        s = Style{}.with_fg(palette().diff_del);
                    else if (!line.empty() && line[0] == '@')
                        s = Style{}.with_fg(palette().info).with_dim();
                    else
                        s = Style{}.with_fg(props.code_fg);
                    code_lines.push_back(text(std::string(line), s));
                } else {
                    code_lines.push_back(text(std::string(line),
                                              Style{}.with_fg(props.code_fg)));
                }
            }
            ++i;
            continue;
        }

        // Code fence opens a block
        if (line.starts_with("```")) {
            process_block(line);
            ++i;
            continue;
        }

        // Block-level lines are processed directly
        if (md_detail::is_block_line(line)) {
            process_block(line);
            ++i;
            continue;
        }

        // Paragraph: collect consecutive non-block lines, join with spaces
        std::string paragraph;
        while (i < src_lines.size() &&
               !md_detail::is_block_line(src_lines[i]) &&
               !src_lines[i].starts_with("```")) {
            if (!paragraph.empty()) paragraph += ' ';
            paragraph += src_lines[i];
            ++i;
        }

        if (!paragraph.empty()) {
            rows.push_back(render_inline(paragraph));
        }
    }

    // Close unclosed code block
    if (in_code_block && !code_lines.empty()) {
        rows.push_back(vstack().bg(props.code_bg).padding(0, 1, 0, 1)(std::move(code_lines)));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
