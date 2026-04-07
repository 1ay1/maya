#include "maya/widget/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"

namespace maya {

// ============================================================================
// Inline parser — parse bold, italic, code, links within a line
// ============================================================================

namespace {

// Find the closing delimiter, handling nesting
size_t find_closing(std::string_view text, std::string_view delim, size_t start) {
    for (size_t i = start; i + delim.size() <= text.size(); ++i) {
        if (text.substr(i, delim.size()) == delim)
            return i;
    }
    return std::string_view::npos;
}

std::vector<md::Inline> parse_inlines(std::string_view text) {
    std::vector<md::Inline> result;
    size_t i = 0;

    while (i < text.size()) {
        // Inline code: `code`
        if (text[i] == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                result.push_back(md::Code{std::string{text.substr(i + 1, end - i - 1)}});
                i = end + 1;
                continue;
            }
        }

        // Bold: **text** or __text__
        if (i + 1 < text.size() &&
            ((text[i] == '*' && text[i + 1] == '*') ||
             (text[i] == '_' && text[i + 1] == '_'))) {
            auto delim = text.substr(i, 2);
            size_t end = find_closing(text, delim, i + 2);
            if (end != std::string_view::npos) {
                auto inner = text.substr(i + 2, end - i - 2);
                result.push_back(md::Bold{parse_inlines(inner)});
                i = end + 2;
                continue;
            }
        }

        // Strikethrough: ~~text~~
        if (i + 1 < text.size() && text[i] == '~' && text[i + 1] == '~') {
            size_t end = find_closing(text, "~~", i + 2);
            if (end != std::string_view::npos) {
                auto inner = text.substr(i + 2, end - i - 2);
                result.push_back(md::Strike{parse_inlines(inner)});
                i = end + 2;
                continue;
            }
            // Unmatched ~~ — consume as plain text
            result.push_back(md::Text{"~~"});
            i += 2;
            continue;
        }

        // Italic: *text* or _text_ (single delimiter)
        if (text[i] == '*' || text[i] == '_') {
            char delim_ch = text[i];
            // Make sure it's not the start of bold
            if (i + 1 < text.size() && text[i + 1] != delim_ch) {
                size_t end = text.find(delim_ch, i + 1);
                if (end != std::string_view::npos) {
                    auto inner = text.substr(i + 1, end - i - 1);
                    result.push_back(md::Italic{parse_inlines(inner)});
                    i = end + 1;
                    continue;
                }
            }
            // Unmatched delimiter — consume as plain text to avoid infinite loop
            size_t run = 1;
            while (i + run < text.size() && text[i + run] == delim_ch) ++run;
            result.push_back(md::Text{std::string{text.substr(i, run)}});
            i += run;
            continue;
        }

        // Link: [text](url)
        if (text[i] == '[') {
            size_t close_bracket = text.find(']', i + 1);
            if (close_bracket != std::string_view::npos &&
                close_bracket + 1 < text.size() &&
                text[close_bracket + 1] == '(') {
                size_t close_paren = text.find(')', close_bracket + 2);
                if (close_paren != std::string_view::npos) {
                    auto link_text = text.substr(i + 1, close_bracket - i - 1);
                    auto url = text.substr(close_bracket + 2,
                                          close_paren - close_bracket - 2);
                    result.push_back(md::Link{
                        std::string{link_text}, std::string{url}});
                    i = close_paren + 1;
                    continue;
                }
            }
            // Unmatched [ — consume as plain text
            result.push_back(md::Text{"["});
            ++i;
            continue;
        }

        // Unmatched special character (backtick, tilde) — consume as text
        if (text[i] == '`' || text[i] == '~') {
            result.push_back(md::Text{std::string{1, text[i]}});
            ++i;
            continue;
        }

        // Plain text: consume until next special character
        size_t start = i;
        while (i < text.size() &&
               text[i] != '`' && text[i] != '*' && text[i] != '_' &&
               text[i] != '~' && text[i] != '[') {
            ++i;
        }
        if (i > start) {
            result.push_back(md::Text{std::string{text.substr(start, i - start)}});
        }
    }

    return result;
}

// Trim leading and trailing whitespace
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

} // anonymous namespace

// ============================================================================
// Block parser — line-oriented markdown parsing
// ============================================================================

md::Document parse_markdown(std::string_view source) {
    md::Document doc;

    // Split into lines
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < source.size()) {
        size_t nl = source.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.push_back(source.substr(pos));
            break;
        }
        lines.push_back(source.substr(pos, nl - pos));
        pos = nl + 1;
    }

    size_t i = 0;
    std::string paragraph_buf;

    auto flush_paragraph = [&] {
        if (!paragraph_buf.empty()) {
            auto trimmed = trim(paragraph_buf);
            if (!trimmed.empty()) {
                doc.blocks.push_back(md::Paragraph{parse_inlines(trimmed)});
            }
            paragraph_buf.clear();
        }
    };

    while (i < lines.size()) {
        auto line = lines[i];

        // Remove trailing CR
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        // Blank line
        if (trim(line).empty()) {
            flush_paragraph();
            ++i;
            continue;
        }

        // Heading: # ... ######
        if (line.size() >= 2 && line[0] == '#') {
            flush_paragraph();
            int level = 0;
            size_t j = 0;
            while (j < line.size() && line[j] == '#' && level < 6) {
                ++level; ++j;
            }
            if (j < line.size() && line[j] == ' ') ++j;
            doc.blocks.push_back(md::Heading{level, parse_inlines(line.substr(j))});
            ++i;
            continue;
        }

        // Fenced code block: ```lang
        if (starts_with(line, "```")) {
            flush_paragraph();
            auto lang = std::string{trim(line.substr(3))};
            std::string code;
            ++i;
            while (i < lines.size()) {
                auto cl = lines[i];
                if (!cl.empty() && cl.back() == '\r') cl.remove_suffix(1);
                if (starts_with(cl, "```")) { ++i; break; }
                if (!code.empty()) code += '\n';
                code += cl;
                ++i;
            }
            doc.blocks.push_back(md::CodeBlock{std::move(code), std::move(lang)});
            continue;
        }

        // Horizontal rule: --- or *** or ___
        if (line.size() >= 3 &&
            (line == "---" || line == "***" || line == "___" ||
             starts_with(line, "---") || starts_with(line, "***"))) {
            auto t = trim(line);
            bool is_rule = t.size() >= 3;
            char first = t[0];
            if (is_rule && (first == '-' || first == '*' || first == '_')) {
                bool all_same = true;
                for (char c : t) {
                    if (c != first && c != ' ') { all_same = false; break; }
                }
                if (all_same) {
                    flush_paragraph();
                    doc.blocks.push_back(md::HRule{});
                    ++i;
                    continue;
                }
            }
        }

        // Blockquote: > text
        if (line.size() >= 2 && line[0] == '>') {
            flush_paragraph();
            std::string bq_text;
            while (i < lines.size()) {
                auto bl = lines[i];
                if (!bl.empty() && bl.back() == '\r') bl.remove_suffix(1);
                if (bl.empty() || bl[0] != '>') break;
                auto content = bl.substr(1);
                if (!content.empty() && content[0] == ' ') content.remove_prefix(1);
                if (!bq_text.empty()) bq_text += '\n';
                bq_text += content;
                ++i;
            }
            auto inner = parse_markdown(bq_text);
            doc.blocks.push_back(md::Blockquote{std::move(inner.blocks)});
            continue;
        }

        // Unordered list: - item or * item
        if (line.size() >= 2 &&
            (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
            line[1] == ' ') {
            flush_paragraph();
            std::vector<md::ListItem> items;
            while (i < lines.size()) {
                auto ll = lines[i];
                if (!ll.empty() && ll.back() == '\r') ll.remove_suffix(1);
                if (ll.size() >= 2 &&
                    (ll[0] == '-' || ll[0] == '*' || ll[0] == '+') &&
                    ll[1] == ' ') {
                    items.push_back(md::ListItem{parse_inlines(ll.substr(2))});
                    ++i;
                } else {
                    break;
                }
            }
            doc.blocks.push_back(md::List{std::move(items), false});
            continue;
        }

        // Ordered list: 1. item
        if (line.size() >= 3 && std::isdigit(static_cast<unsigned char>(line[0]))) {
            size_t dot = line.find('.');
            if (dot != std::string_view::npos && dot + 1 < line.size() &&
                line[dot + 1] == ' ') {
                // Check all chars before dot are digits
                bool all_digits = true;
                for (size_t k = 0; k < dot; ++k) {
                    if (!std::isdigit(static_cast<unsigned char>(line[k]))) {
                        all_digits = false; break;
                    }
                }
                if (all_digits) {
                    flush_paragraph();
                    std::vector<md::ListItem> items;
                    while (i < lines.size()) {
                        auto ll = lines[i];
                        if (!ll.empty() && ll.back() == '\r') ll.remove_suffix(1);
                        size_t d = ll.find('.');
                        if (d != std::string_view::npos && d + 1 < ll.size() &&
                            ll[d + 1] == ' ') {
                            bool ok = true;
                            for (size_t k = 0; k < d; ++k) {
                                if (!std::isdigit(static_cast<unsigned char>(ll[k]))) {
                                    ok = false; break;
                                }
                            }
                            if (ok) {
                                items.push_back(md::ListItem{parse_inlines(ll.substr(d + 2))});
                                ++i;
                                continue;
                            }
                        }
                        break;
                    }
                    doc.blocks.push_back(md::List{std::move(items), true});
                    continue;
                }
            }
        }

        // Regular paragraph text
        if (!paragraph_buf.empty()) paragraph_buf += ' ';
        paragraph_buf += line;
        ++i;
    }

    flush_paragraph();
    return doc;
}

// ============================================================================
// AST to Element conversion
// ============================================================================

Element md_inline_to_element(const md::Inline& span) {
    return std::visit(overload{
        [](const md::Text& t) -> Element {
            return Element{TextElement{.content = t.content}};
        },
        [](const md::Bold& b) -> Element {
            std::vector<Element> children;
            for (auto& child : b.children)
                children.push_back(md_inline_to_element(child));
            auto box = detail::hstack();
            return box(std::move(children))
                | Style{}.with_bold();
        },
        [](const md::Italic& it) -> Element {
            std::vector<Element> children;
            for (auto& child : it.children)
                children.push_back(md_inline_to_element(child));
            auto box = detail::hstack();
            return box(std::move(children))
                | Style{}.with_italic();
        },
        [](const md::Code& c) -> Element {
            return Element{TextElement{
                .content = c.content,
                .style = Style{}
                    .with_fg(Color::rgb(230, 219, 116))
                    .with_bg(Color::rgb(40, 40, 50)),
            }};
        },
        [](const md::Link& l) -> Element {
            // OSC 8 hyperlinks could be added here in the future
            return Element{TextElement{
                .content = l.text,
                .style = Style{}
                    .with_fg(Color::rgb(100, 149, 237))
                    .with_underline(),
            }};
        },
        [](const md::Strike& s) -> Element {
            std::vector<Element> children;
            for (auto& child : s.children)
                children.push_back(md_inline_to_element(child));
            auto box = detail::hstack();
            return box(std::move(children))
                | Style{}.with_strikethrough();
        },
    }, span.inner);
}

Element md_block_to_element(const md::Block& block) {
    return std::visit(overload{
        [](const md::Paragraph& p) -> Element {
            std::vector<Element> spans;
            for (auto& s : p.spans)
                spans.push_back(md_inline_to_element(s));
            return detail::hstack()(std::move(spans));
        },
        [](const md::Heading& h) -> Element {
            std::vector<Element> spans;
            for (auto& s : h.spans)
                spans.push_back(md_inline_to_element(s));
            // Style based on heading level
            Style sty = Style{}.with_bold();
            switch (h.level) {
                case 1: sty = sty.with_fg(Color::rgb(255, 255, 255)); break;
                case 2: sty = sty.with_fg(Color::rgb(200, 200, 255)); break;
                case 3: sty = sty.with_fg(Color::rgb(180, 180, 220)); break;
                default: sty = sty.with_fg(Color::rgb(160, 160, 200)); break;
            }
            // Prefix with # symbols
            std::string prefix(static_cast<size_t>(h.level), '#');
            prefix += ' ';
            spans.insert(spans.begin(), Element{TextElement{
                .content = std::move(prefix),
                .style = sty.with_dim(),
            }});
            auto row = detail::hstack()(std::move(spans));
            return row | sty;
        },
        [](const md::CodeBlock& c) -> Element {
            return detail::vstack()
                .border(BorderStyle::Round)
                .border_color(Color::rgb(60, 65, 80))
                .padding(0, 1, 0, 1)(
                    Element{TextElement{
                        .content = c.content,
                        .style = Style{}.with_fg(Color::rgb(220, 220, 220)),
                    }}
                );
        },
        [](const md::Blockquote& bq) -> Element {
            std::vector<Element> children;
            for (auto& child : bq.children)
                children.push_back(md_block_to_element(child));
            return detail::hstack()(
                Element{TextElement{
                    .content = "\xe2\x94\x82 ",  // "│ "
                    .style = Style{}.with_fg(Color::rgb(80, 80, 120)),
                }},
                detail::vstack()(std::move(children))
            );
        },
        [](const md::List& l) -> Element {
            std::vector<Element> items;
            int num = 1;
            for (auto& item : l.items) {
                std::string prefix;
                if (l.ordered) {
                    prefix = std::to_string(num++) + ". ";
                } else {
                    prefix = "  \xe2\x80\xa2 ";  // "  \u2022 "
                }
                std::vector<Element> spans;
                spans.push_back(Element{TextElement{
                    .content = std::move(prefix),
                    .style = Style{}.with_dim(),
                }});
                for (auto& s : item.spans)
                    spans.push_back(md_inline_to_element(s));
                items.push_back(detail::hstack()(std::move(spans)));
            }
            return detail::vstack()(std::move(items));
        },
        [](const md::HRule&) -> Element {
            return detail::box()
                .border(BorderStyle::Single)
                .border_sides(BorderSides::horizontal());
        },
    }, block.inner);
}

Element markdown(std::string_view markdown) {
    auto doc = parse_markdown(markdown);
    if (doc.blocks.empty()) return Element{TextElement{""}};

    std::vector<Element> blocks;
    blocks.reserve(doc.blocks.size());
    for (auto& block : doc.blocks)
        blocks.push_back(md_block_to_element(block));

    return detail::vstack().gap(1)(std::move(blocks));
}

} // namespace maya
