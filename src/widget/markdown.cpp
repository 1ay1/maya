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
// Inline parser — single-pass, stack-based delimiter matching
// ============================================================================

namespace {

// Find the closing delimiter (linear scan — called only when open found).
size_t find_closing(std::string_view text, std::string_view delim, size_t start) {
    for (size_t i = start; i + delim.size() <= text.size(); ++i) {
        if (text.substr(i, delim.size()) == delim)
            return i;
    }
    return std::string_view::npos;
}

// Coalesce adjacent Text nodes into one to reduce element tree depth.
void push_text(std::vector<md::Inline>& result, std::string_view sv) {
    if (sv.empty()) return;
    if (!result.empty()) {
        auto* prev = std::get_if<md::Text>(&result.back().inner);
        if (prev) {
            prev->content += sv;
            return;
        }
    }
    result.push_back(md::Text{std::string{sv}});
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
            push_text(result, "~~");
            i += 2;
            continue;
        }

        // Italic: *text* or _text_ (single delimiter)
        if (text[i] == '*' || text[i] == '_') {
            char delim_ch = text[i];
            if (i + 1 < text.size() && text[i + 1] != delim_ch) {
                size_t end = text.find(delim_ch, i + 1);
                if (end != std::string_view::npos) {
                    auto inner = text.substr(i + 1, end - i - 1);
                    result.push_back(md::Italic{parse_inlines(inner)});
                    i = end + 1;
                    continue;
                }
            }
            // Unmatched delimiter — consume as plain text
            size_t run = 1;
            while (i + run < text.size() && text[i + run] == delim_ch) ++run;
            push_text(result, text.substr(i, run));
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
            push_text(result, "[");
            ++i;
            continue;
        }

        // Unmatched special character
        if (text[i] == '`' || text[i] == '~') {
            push_text(result, text.substr(i, 1));
            ++i;
            continue;
        }

        // Plain text: consume until next special character (batch scan)
        size_t start = i;
        while (i < text.size() &&
               text[i] != '`' && text[i] != '*' && text[i] != '_' &&
               text[i] != '~' && text[i] != '[') {
            ++i;
        }
        if (i > start) {
            push_text(result, text.substr(start, i - start));
        }
    }

    return result;
}

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

    // Split into lines — single pass, no allocation for the views themselves
    std::vector<std::string_view> lines;
    lines.reserve(32); // reasonable default for typical markdown
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
// AST to Element conversion — polished terminal rendering
// ============================================================================

// Color palette for consistent, beautiful output
namespace colors {
    constexpr auto text       = Color::rgb(220, 220, 235);
    constexpr auto heading1   = Color::rgb(255, 255, 255);
    constexpr auto heading2   = Color::rgb(190, 190, 255);
    constexpr auto heading3   = Color::rgb(170, 170, 230);
    constexpr auto heading_dim= Color::rgb(100, 100, 140);
    constexpr auto bold_fg    = Color::rgb(255, 255, 255);
    constexpr auto italic_fg  = Color::rgb(200, 200, 230);
    constexpr auto code_fg    = Color::rgb(245, 220, 120);
    constexpr auto code_bg    = Color::rgb(35, 35, 48);
    constexpr auto link_fg    = Color::rgb(100, 160, 255);
    constexpr auto strike_fg  = Color::rgb(120, 120, 140);
    constexpr auto quote_bar  = Color::rgb(80, 80, 140);
    constexpr auto quote_text = Color::rgb(180, 180, 210);
    constexpr auto list_bullet= Color::rgb(100, 180, 255);
    constexpr auto list_num   = Color::rgb(100, 180, 255);
    constexpr auto code_border= Color::rgb(55, 55, 75);
    constexpr auto code_lang  = Color::rgb(130, 130, 170);
    constexpr auto hrule_fg   = Color::rgb(60, 60, 85);
}

Element md_inline_to_element(const md::Inline& span) {
    return std::visit(overload{
        [](const md::Text& t) -> Element {
            return Element{TextElement{.content = t.content}};
        },
        [](const md::Bold& b) -> Element {
            if (b.children.size() == 1) {
                // Single child — skip hstack wrapper
                auto child = md_inline_to_element(b.children[0]);
                return child | Style{}.with_bold().with_fg(colors::bold_fg);
            }
            std::vector<Element> children;
            children.reserve(b.children.size());
            for (auto& child : b.children)
                children.push_back(md_inline_to_element(child));
            return detail::hstack()(std::move(children))
                | Style{}.with_bold().with_fg(colors::bold_fg);
        },
        [](const md::Italic& it) -> Element {
            if (it.children.size() == 1) {
                auto child = md_inline_to_element(it.children[0]);
                return child | Style{}.with_italic().with_fg(colors::italic_fg);
            }
            std::vector<Element> children;
            children.reserve(it.children.size());
            for (auto& child : it.children)
                children.push_back(md_inline_to_element(child));
            return detail::hstack()(std::move(children))
                | Style{}.with_italic().with_fg(colors::italic_fg);
        },
        [](const md::Code& c) -> Element {
            // Inline code with subtle background
            return Element{TextElement{
                .content = " " + c.content + " ",
                .style = Style{}
                    .with_fg(colors::code_fg)
                    .with_bg(colors::code_bg),
            }};
        },
        [](const md::Link& l) -> Element {
            return Element{TextElement{
                .content = l.text,
                .style = Style{}
                    .with_fg(colors::link_fg)
                    .with_underline(),
            }};
        },
        [](const md::Strike& s) -> Element {
            if (s.children.size() == 1) {
                auto child = md_inline_to_element(s.children[0]);
                return child | Style{}.with_strikethrough().with_fg(colors::strike_fg);
            }
            std::vector<Element> children;
            children.reserve(s.children.size());
            for (auto& child : s.children)
                children.push_back(md_inline_to_element(child));
            return detail::hstack()(std::move(children))
                | Style{}.with_strikethrough().with_fg(colors::strike_fg);
        },
    }, span.inner);
}

// Build inline spans into an element — avoids hstack for single spans.
static Element build_inline_row(const std::vector<md::Inline>& spans) {
    if (spans.empty()) return Element{TextElement{}};
    if (spans.size() == 1) return md_inline_to_element(spans[0]);
    std::vector<Element> elems;
    elems.reserve(spans.size());
    for (auto& s : spans)
        elems.push_back(md_inline_to_element(s));
    return detail::hstack()(std::move(elems));
}

Element md_block_to_element(const md::Block& block) {
    return std::visit(overload{
        [](const md::Paragraph& p) -> Element {
            return build_inline_row(p.spans);
        },
        [](const md::Heading& h) -> Element {
            // Style progression: h1=bright white bold, h2=blue-white, h3=dimmer
            Style sty = Style{}.with_bold();
            switch (h.level) {
                case 1: sty = sty.with_fg(colors::heading1); break;
                case 2: sty = sty.with_fg(colors::heading2); break;
                case 3: sty = sty.with_fg(colors::heading3); break;
                default: sty = sty.with_fg(colors::heading3).with_dim(); break;
            }

            std::string prefix(static_cast<size_t>(h.level), '#');
            prefix += ' ';

            if (h.spans.size() == 1) {
                // Fast path: single span heading
                auto child = md_inline_to_element(h.spans[0]);
                return detail::hstack()(
                    Element{TextElement{
                        .content = std::move(prefix),
                        .style = Style{}.with_fg(colors::heading_dim).with_bold(),
                    }},
                    child | sty
                );
            }

            std::vector<Element> spans;
            spans.reserve(h.spans.size() + 1);
            spans.push_back(Element{TextElement{
                .content = std::move(prefix),
                .style = Style{}.with_fg(colors::heading_dim).with_bold(),
            }});
            for (auto& s : h.spans)
                spans.push_back(md_inline_to_element(s));
            auto row = detail::hstack()(std::move(spans));
            return row | sty;
        },
        [](const md::CodeBlock& c) -> Element {
            // Code block with rounded border and language label
            auto builder = detail::vstack()
                .border(BorderStyle::Round)
                .border_color(colors::code_border)
                .padding(0, 1, 0, 1);

            if (!c.lang.empty()) {
                builder = std::move(builder).border_text(
                    " " + c.lang + " ",
                    BorderTextPos::Top,
                    BorderTextAlign::Start);
            }

            return builder(
                Element{TextElement{
                    .content = c.content,
                    .style = Style{}.with_fg(colors::text),
                }}
            );
        },
        [](const md::Blockquote& bq) -> Element {
            std::vector<Element> children;
            children.reserve(bq.children.size());
            for (auto& child : bq.children)
                children.push_back(md_block_to_element(child));

            // Colored bar with italic quote text
            return detail::hstack()(
                Element{TextElement{
                    .content = "\xe2\x94\x82 ",  // "│ "
                    .style = Style{}.with_fg(colors::quote_bar),
                }},
                detail::vstack()(std::move(children))
                    | Style{}.with_italic().with_fg(colors::quote_text)
            );
        },
        [](const md::List& l) -> Element {
            std::vector<Element> items;
            items.reserve(l.items.size());
            int num = 1;
            for (auto& item : l.items) {
                std::string prefix;
                Style prefix_style;
                if (l.ordered) {
                    prefix = std::to_string(num++) + ". ";
                    prefix_style = Style{}.with_fg(colors::list_num).with_bold();
                } else {
                    prefix = "  \xe2\x80\xa2 ";  // "  • "
                    prefix_style = Style{}.with_fg(colors::list_bullet);
                }

                auto content = build_inline_row(item.spans);
                items.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = std::move(prefix),
                        .style = prefix_style,
                    }},
                    std::move(content)
                ));
            }
            return detail::vstack()(std::move(items));
        },
        [](const md::HRule&) -> Element {
            // Simple text-based rule — avoids ComponentElement overhead
            return Element{TextElement{
                .content = "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",  // 20× "─"
                .style = Style{}.with_fg(colors::hrule_fg),
            }};
        },
    }, block.inner);
}

Element markdown(std::string_view source) {
    auto doc = parse_markdown(source);
    if (doc.blocks.empty()) return Element{TextElement{""}};

    // Single block — skip vstack wrapper entirely
    if (doc.blocks.size() == 1)
        return md_block_to_element(doc.blocks[0]);

    std::vector<Element> blocks;
    blocks.reserve(doc.blocks.size());
    for (auto& block : doc.blocks)
        blocks.push_back(md_block_to_element(block));

    return detail::vstack().gap(1)(std::move(blocks));
}

// ============================================================================
// StreamingMarkdown — progressive per-block rendering
// ============================================================================

size_t StreamingMarkdown::find_block_boundary() const noexcept {
    // Walk forward from committed_ tracking code fence state, find the
    // last position where a complete block ends.  A block boundary is a
    // blank line (\n\n) that is NOT inside a code fence.
    bool in_fence = in_code_fence_;
    size_t last_boundary = committed_;

    size_t i = committed_;
    while (i < source_.size()) {
        // Check for code fence toggle
        if (i + 3 <= source_.size() &&
            source_[i] == '`' && source_[i+1] == '`' && source_[i+2] == '`') {
            // Find end of this line
            size_t eol = source_.find('\n', i);
            if (eol == std::string::npos) break; // incomplete fence line
            in_fence = !in_fence;
            i = eol + 1;
            if (!in_fence) {
                // Just closed a code fence — this is a block boundary
                last_boundary = i;
            }
            continue;
        }

        // Check for blank line (block separator) outside code fences
        if (!in_fence && source_[i] == '\n') {
            size_t next = i + 1;
            if (next < source_.size() && source_[next] == '\n') {
                // Double newline — block boundary after it
                last_boundary = next + 1;
                i = next + 1;
                continue;
            }
            // Single newline + block-level marker = boundary
            if (next < source_.size()) {
                char c = source_[next];
                if (c == '#' || c == '>' || c == '-' || c == '*' || c == '+') {
                    last_boundary = next;
                }
            }
        }

        // Advance to next line
        size_t eol = source_.find('\n', i);
        if (eol == std::string::npos) break;
        i = eol + 1;
    }

    return last_boundary;
}

void StreamingMarkdown::set_content(std::string_view content) {
    if (content.size() >= source_.size() &&
        content.substr(0, source_.size()) == source_) {
        // Pure append — fast path
        if (content.size() > source_.size()) {
            source_ = std::string{content};
        }
    } else {
        // Content changed (shouldn't happen in streaming, but handle it)
        clear();
        source_ = std::string{content};
    }

    // Find new complete blocks and parse them
    size_t boundary = find_block_boundary();
    if (boundary > committed_) {
        auto new_text = std::string_view{source_}.substr(committed_, boundary - committed_);
        auto doc = parse_markdown(new_text);
        for (auto& block : doc.blocks) {
            blocks_.push_back(md_block_to_element(block));
        }
        // Update fence state by counting fences in the committed region
        for (size_t j = committed_; j < boundary; ++j) {
            if (j + 3 <= boundary &&
                source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') {
                in_code_fence_ = !in_code_fence_;
            }
        }
        committed_ = boundary;
    }
}

void StreamingMarkdown::append(std::string_view text) {
    source_ += text;

    size_t boundary = find_block_boundary();
    if (boundary > committed_) {
        auto new_text = std::string_view{source_}.substr(committed_, boundary - committed_);
        auto doc = parse_markdown(new_text);
        for (auto& block : doc.blocks) {
            blocks_.push_back(md_block_to_element(block));
        }
        for (size_t j = committed_; j < boundary; ++j) {
            if (j + 3 <= boundary &&
                source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') {
                in_code_fence_ = !in_code_fence_;
            }
        }
        committed_ = boundary;
    }
}

void StreamingMarkdown::finish() {
    if (committed_ < source_.size()) {
        auto tail = std::string_view{source_}.substr(committed_);
        auto doc = parse_markdown(tail);
        for (auto& block : doc.blocks) {
            blocks_.push_back(md_block_to_element(block));
        }
        committed_ = source_.size();
        in_code_fence_ = false;
    }
}

void StreamingMarkdown::clear() {
    source_.clear();
    committed_ = 0;
    blocks_.clear();
    in_code_fence_ = false;
}

Element StreamingMarkdown::build() const {
    // Combine cached blocks + raw tail
    std::string_view tail;
    if (committed_ < source_.size()) {
        tail = std::string_view{source_}.substr(committed_);
    }

    bool has_tail = !tail.empty();
    size_t total = blocks_.size() + (has_tail ? 1 : 0);

    if (total == 0) return Element{TextElement{""}};

    if (total == 1 && !has_tail) return blocks_[0];

    if (total == 1 && blocks_.empty()) {
        // Only tail, no cached blocks — render as styled plain text
        return Element{TextElement{
            .content = std::string{tail},
            .style = Style{}.with_fg(Color::rgb(220, 220, 235)),
        }};
    }

    std::vector<Element> all;
    all.reserve(total);
    for (auto& b : blocks_) all.push_back(b);

    if (has_tail) {
        // Render the in-progress tail as plain styled text
        all.push_back(Element{TextElement{
            .content = std::string{tail},
            .style = Style{}.with_fg(Color::rgb(220, 220, 235)),
        }});
    }

    return detail::vstack().gap(1)(std::move(all));
}

} // namespace maya
