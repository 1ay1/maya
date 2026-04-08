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
// Utility helpers
// ============================================================================

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Escapable characters per CommonMark spec
bool is_escapable(char c) {
    return c == '\\' || c == '`' || c == '*' || c == '_' || c == '{' ||
           c == '}' || c == '[' || c == ']' || c == '(' || c == ')' ||
           c == '#' || c == '+' || c == '-' || c == '.' || c == '!' ||
           c == '|' || c == '~' || c == '<' || c == '>' || c == '"' ||
           c == '\'' || c == '^';
}

// ============================================================================
// Inline parser — single-pass, stack-based delimiter matching
// ============================================================================

// Find the closing delimiter (linear scan — called only when open found).
// Respects backslash escapes.
size_t find_closing(std::string_view text, std::string_view delim, size_t start) {
    for (size_t i = start; i + delim.size() <= text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) { ++i; continue; }
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

void push_char(std::vector<md::Inline>& result, char c) {
    if (!result.empty()) {
        auto* prev = std::get_if<md::Text>(&result.back().inner);
        if (prev) {
            prev->content += c;
            return;
        }
    }
    result.push_back(md::Text{std::string(1, c)});
}

std::vector<md::Inline> parse_inlines(std::string_view text) {
    std::vector<md::Inline> result;
    size_t i = 0;

    while (i < text.size()) {
        // Backslash escape
        if (text[i] == '\\' && i + 1 < text.size()) {
            if (is_escapable(text[i + 1])) {
                push_char(result, text[i + 1]);
                i += 2;
                continue;
            }
            // Hard line break: backslash before newline
            if (text[i + 1] == '\n') {
                result.push_back(md::HardBreak{});
                i += 2;
                continue;
            }
        }

        // Hard line break: two+ trailing spaces before newline
        if (text[i] == ' ' && i + 2 < text.size()) {
            size_t spaces = 0;
            size_t j = i;
            while (j < text.size() && text[j] == ' ') { ++spaces; ++j; }
            if (spaces >= 2 && j < text.size() && text[j] == '\n') {
                result.push_back(md::HardBreak{});
                i = j + 1;
                continue;
            }
        }

        // Inline code: `code` or ``code``
        if (text[i] == '`') {
            // Count opening backticks
            size_t ticks = 0;
            size_t j = i;
            while (j < text.size() && text[j] == '`') { ++ticks; ++j; }
            // Find matching closing backticks
            auto closing = std::string(ticks, '`');
            size_t end = text.find(std::string_view{closing}, j);
            if (end != std::string_view::npos) {
                auto code = text.substr(j, end - j);
                // Strip one leading/trailing space if both present (CommonMark)
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') {
                    code.remove_prefix(1);
                    code.remove_suffix(1);
                }
                result.push_back(md::Code{std::string{code}});
                i = end + ticks;
                continue;
            }
            // No closing — emit as text
            push_text(result, text.substr(i, ticks));
            i = j;
            continue;
        }

        // Bold+italic: ***text*** or ___text___
        if (i + 2 < text.size() &&
            ((text[i] == '*' && text[i+1] == '*' && text[i+2] == '*') ||
             (text[i] == '_' && text[i+1] == '_' && text[i+2] == '_'))) {
            auto delim = text.substr(i, 3);
            size_t end = find_closing(text, delim, i + 3);
            if (end != std::string_view::npos) {
                auto inner = text.substr(i + 3, end - i - 3);
                result.push_back(md::BoldItalic{parse_inlines(inner)});
                i = end + 3;
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
            if (i + 1 < text.size() && text[i + 1] != delim_ch && text[i + 1] != ' ') {
                size_t end = text.find(delim_ch, i + 1);
                if (end != std::string_view::npos && text[end - 1] != ' ') {
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

        // Image: ![alt](url)
        if (text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[') {
            size_t close_bracket = text.find(']', i + 2);
            if (close_bracket != std::string_view::npos &&
                close_bracket + 1 < text.size() &&
                text[close_bracket + 1] == '(') {
                size_t close_paren = text.find(')', close_bracket + 2);
                if (close_paren != std::string_view::npos) {
                    auto alt = text.substr(i + 2, close_bracket - i - 2);
                    auto url = text.substr(close_bracket + 2,
                                          close_paren - close_bracket - 2);
                    result.push_back(md::Image{std::string{alt}, std::string{url}});
                    i = close_paren + 1;
                    continue;
                }
            }
            push_text(result, "!");
            ++i;
            continue;
        }

        // Link: [text](url)
        if (text[i] == '[') {
            // Footnote reference: [^label]
            if (i + 1 < text.size() && text[i + 1] == '^') {
                size_t close = text.find(']', i + 2);
                if (close != std::string_view::npos) {
                    auto label = text.substr(i + 2, close - i - 2);
                    // Only if it's a pure reference (no (url) after)
                    if (close + 1 >= text.size() || text[close + 1] != '(') {
                        result.push_back(md::FootnoteRef{std::string{label}});
                        i = close + 1;
                        continue;
                    }
                }
            }

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

        // Autolink: <url> or <email>
        if (text[i] == '<') {
            size_t close = text.find('>', i + 1);
            if (close != std::string_view::npos) {
                auto content = text.substr(i + 1, close - i - 1);
                // Check if it looks like a URL or email
                bool is_url = content.find("://") != std::string_view::npos;
                bool is_email = content.find('@') != std::string_view::npos &&
                                content.find(' ') == std::string_view::npos;
                if (is_url || is_email) {
                    std::string url_str{content};
                    if (is_email && !starts_with(content, "mailto:"))
                        url_str = "mailto:" + url_str;
                    result.push_back(md::Link{std::string{content}, std::move(url_str)});
                    i = close + 1;
                    continue;
                }
            }
            push_text(result, "<");
            ++i;
            continue;
        }

        // Unmatched special characters (! without [, lone ~)
        if (text[i] == '~' || text[i] == '!') {
            push_text(result, text.substr(i, 1));
            ++i;
            continue;
        }

        // Plain text: consume until next special character (batch scan)
        size_t start = i;
        while (i < text.size() &&
               text[i] != '`' && text[i] != '*' && text[i] != '_' &&
               text[i] != '~' && text[i] != '[' && text[i] != '!' &&
               text[i] != '\\' && text[i] != '<') {
            // Also break on trailing spaces before newline (hard break detection)
            if (text[i] == ' ' && i + 2 < text.size()) {
                size_t j = i;
                while (j < text.size() && text[j] == ' ') ++j;
                if (j < text.size() && text[j] == '\n' && (j - i) >= 2) break;
            }
            ++i;
        }
        if (i > start) {
            push_text(result, text.substr(start, i - start));
        }
    }

    return result;
}

// ============================================================================
// Table helpers
// ============================================================================

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

// ============================================================================
// List parsing helpers
// ============================================================================

// How many spaces of indentation does a line have?
int count_indent(std::string_view line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

// Remove up to `n` spaces of indentation
std::string_view dedent(std::string_view line, int n) {
    int removed = 0;
    size_t i = 0;
    while (i < line.size() && removed < n) {
        if (line[i] == ' ') { ++removed; ++i; }
        else if (line[i] == '\t') { removed += 4; ++i; }
        else break;
    }
    return line.substr(i);
}

// Check if a line is an unordered list marker (returns content offset, 0 if not)
int ul_marker_len(std::string_view line) {
    auto t = trim(line);
    if (t.size() >= 2 &&
        (t[0] == '-' || t[0] == '*' || t[0] == '+') &&
        t[1] == ' ') {
        // Find position in original line
        auto offset = static_cast<int>(line.size() - t.size());
        return offset + 2;
    }
    return 0;
}

// Check if a line is an ordered list marker (returns content offset, 0 if not)
int ol_marker_len(std::string_view line) {
    auto t = trim(line);
    if (t.size() < 3) return 0;
    size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    if (d == 0 || d >= t.size()) return 0;
    if ((t[d] == '.' || t[d] == ')') && d + 1 < t.size() && t[d + 1] == ' ') {
        auto offset = static_cast<int>(line.size() - t.size());
        return offset + static_cast<int>(d) + 2;
    }
    return 0;
}

// Extract the starting number from an ordered list line
int ol_start_num(std::string_view line) {
    auto t = trim(line);
    size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    int num = 0;
    for (size_t k = 0; k < d; ++k)
        num = num * 10 + (t[k] - '0');
    return num;
}

// Check for task list checkbox: "[ ] " or "[x] " or "[X] "
// Returns: -1 = not a task, 0 = unchecked, 1 = checked
int parse_task_checkbox(std::string_view content) {
    if (content.size() >= 4 && content[0] == '[' && content[2] == ']' && content[3] == ' ') {
        if (content[1] == ' ') return 0;
        if (content[1] == 'x' || content[1] == 'X') return 1;
    }
    return -1;
}

} // anonymous namespace

// ============================================================================
// Block parser — line-oriented markdown parsing
// ============================================================================

md::Document parse_markdown(std::string_view source) {
    md::Document doc;

    // Split into lines
    std::vector<std::string_view> lines;
    lines.reserve(32);
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

        // Setext heading: check if NEXT line is === or ---
        // (only if we have paragraph_buf accumulating, meaning current line is text)
        if (!paragraph_buf.empty() && i + 1 <= lines.size()) {
            // We're in paragraph mode, check if current continuation + next
            // would form a setext heading. Actually setext is detected when
            // we see the underline, so handle it below after paragraph check.
        }

        // ATX Heading: # ... ######
        if (line.size() >= 2 && line[0] == '#') {
            flush_paragraph();
            int level = 0;
            size_t j = 0;
            while (j < line.size() && line[j] == '#' && level < 6) {
                ++level; ++j;
            }
            if (j < line.size() && line[j] == ' ') ++j;
            // Strip trailing #s (CommonMark)
            auto content = line.substr(j);
            while (content.size() >= 2 && content.back() == '#') content.remove_suffix(1);
            content = trim(content);
            doc.blocks.push_back(md::Heading{level, parse_inlines(content)});
            ++i;
            continue;
        }

        // Fenced code block: ```lang or ~~~lang
        if (starts_with(line, "```") || starts_with(line, "~~~")) {
            flush_paragraph();
            char fence_char = line[0];
            size_t fence_len = 0;
            while (fence_len < line.size() && line[fence_len] == fence_char) ++fence_len;
            auto lang = std::string{trim(line.substr(fence_len))};
            std::string code;
            ++i;
            while (i < lines.size()) {
                auto cl = lines[i];
                if (!cl.empty() && cl.back() == '\r') cl.remove_suffix(1);
                // Closing fence: same char, at least same length
                size_t cl_fence = 0;
                while (cl_fence < cl.size() && cl[cl_fence] == fence_char) ++cl_fence;
                if (cl_fence >= fence_len && trim(cl.substr(cl_fence)).empty()) {
                    ++i;
                    break;
                }
                if (!code.empty()) code += '\n';
                code += cl;
                ++i;
            }
            doc.blocks.push_back(md::CodeBlock{std::move(code), std::move(lang)});
            continue;
        }

        // Indented code block: 4+ spaces (only if not in a list context)
        if (count_indent(line) >= 4 && paragraph_buf.empty()) {
            flush_paragraph();
            std::string code;
            while (i < lines.size()) {
                auto cl = lines[i];
                if (!cl.empty() && cl.back() == '\r') cl.remove_suffix(1);
                if (count_indent(cl) >= 4) {
                    if (!code.empty()) code += '\n';
                    code += dedent(cl, 4);
                    ++i;
                } else if (trim(cl).empty()) {
                    // Blank lines can be part of indented code
                    if (!code.empty()) code += '\n';
                    ++i;
                } else {
                    break;
                }
            }
            // Trim trailing blank lines from code
            while (!code.empty() && code.back() == '\n') code.pop_back();
            doc.blocks.push_back(md::CodeBlock{std::move(code), {}});
            continue;
        }

        // Horizontal rule: ---, ***, ___ (with optional spaces)
        {
            auto t = trim(line);
            if (t.size() >= 3) {
                char first = t[0];
                if (first == '-' || first == '*' || first == '_') {
                    bool all_same = true;
                    int count = 0;
                    for (char c : t) {
                        if (c == first) ++count;
                        else if (c != ' ') { all_same = false; break; }
                    }
                    if (all_same && count >= 3) {
                        // Check it's not a setext heading (--- under paragraph text)
                        if (first == '-' && !paragraph_buf.empty()) {
                            // This is a setext heading level 2
                            auto heading_text = trim(paragraph_buf);
                            doc.blocks.push_back(md::Heading{2, parse_inlines(heading_text)});
                            paragraph_buf.clear();
                            ++i;
                            continue;
                        }
                        flush_paragraph();
                        doc.blocks.push_back(md::HRule{});
                        ++i;
                        continue;
                    }
                }
            }
        }

        // Setext heading level 1: line of ===
        {
            auto t = trim(line);
            if (!t.empty() && t[0] == '=' && !paragraph_buf.empty()) {
                bool all_eq = true;
                for (char c : t) { if (c != '=') { all_eq = false; break; } }
                if (all_eq && t.size() >= 1) {
                    auto heading_text = trim(paragraph_buf);
                    doc.blocks.push_back(md::Heading{1, parse_inlines(heading_text)});
                    paragraph_buf.clear();
                    ++i;
                    continue;
                }
            }
        }

        // Blockquote: > text
        if (line.size() >= 1 && trim(line)[0] == '>') {
            flush_paragraph();
            std::string bq_text;
            while (i < lines.size()) {
                auto bl = lines[i];
                if (!bl.empty() && bl.back() == '\r') bl.remove_suffix(1);
                auto bt = trim(bl);
                if (bt.empty() || bt[0] != '>') break;
                auto content = bt.substr(1);
                if (!content.empty() && content[0] == ' ') content.remove_prefix(1);
                if (!bq_text.empty()) bq_text += '\n';
                bq_text += content;
                ++i;
            }
            auto inner = parse_markdown(bq_text);
            doc.blocks.push_back(md::Blockquote{std::move(inner.blocks)});
            continue;
        }

        // Footnote definition: [^label]: content
        if (starts_with(trim(line), "[^")) {
            auto t = trim(line);
            size_t close = t.find("]:");
            if (close != std::string_view::npos && close > 2) {
                flush_paragraph();
                auto label = std::string{t.substr(2, close - 2)};
                auto first_content = t.substr(close + 2);
                if (!first_content.empty() && first_content[0] == ' ')
                    first_content.remove_prefix(1);

                std::string fn_text{first_content};
                ++i;
                // Continuation lines: indented by 2+ spaces
                while (i < lines.size()) {
                    auto fl = lines[i];
                    if (!fl.empty() && fl.back() == '\r') fl.remove_suffix(1);
                    if (trim(fl).empty()) {
                        fn_text += '\n';
                        ++i;
                        continue;
                    }
                    if (count_indent(fl) >= 2) {
                        fn_text += '\n';
                        fn_text += dedent(fl, 2);
                        ++i;
                    } else {
                        break;
                    }
                }
                auto inner = parse_markdown(fn_text);
                doc.blocks.push_back(md::FootnoteDef{std::move(label), std::move(inner.blocks)});
                continue;
            }
        }

        // Lists: unordered (- * +) and ordered (1. 1))
        {
            int ul_len = ul_marker_len(line);
            int ol_len = ol_marker_len(line);
            if (ul_len > 0 || ol_len > 0) {
                flush_paragraph();
                bool ordered = ol_len > 0;
                int marker_len = ordered ? ol_len : ul_len;
                int start_num = ordered ? ol_start_num(line) : 1;
                int base_indent = count_indent(line);

                std::vector<md::ListItem> items;

                while (i < lines.size()) {
                    auto ll = lines[i];
                    if (!ll.empty() && ll.back() == '\r') ll.remove_suffix(1);

                    int cur_ul = ul_marker_len(ll);
                    int cur_ol = ol_marker_len(ll);
                    bool is_item = ordered ? (cur_ol > 0) : (cur_ul > 0);
                    int cur_indent = count_indent(ll);

                    // Only match items at the same indentation level
                    if (!is_item || std::abs(cur_indent - base_indent) > 1) {
                        // Could be a continuation or sub-list
                        if (trim(ll).empty() || cur_indent > base_indent + 1) {
                            // Continuation or nested content — append to last item
                            if (!items.empty()) {
                                // Collect all continuation/nested lines
                                std::string sub_text;
                                while (i < lines.size()) {
                                    auto sl = lines[i];
                                    if (!sl.empty() && sl.back() == '\r') sl.remove_suffix(1);
                                    int si = count_indent(sl);
                                    bool blank = trim(sl).empty();

                                    // A non-indented non-blank line that's not a list
                                    // marker at higher indent = end of this item
                                    if (!blank && si <= base_indent) {
                                        int sul = ul_marker_len(sl);
                                        int sol = ol_marker_len(sl);
                                        if ((ordered && sol > 0 && si == base_indent) ||
                                            (!ordered && sul > 0 && si == base_indent)) {
                                            break; // next sibling item
                                        }
                                        if (si <= base_indent) break; // end of list
                                    }

                                    if (!sub_text.empty()) sub_text += '\n';
                                    sub_text += dedent(sl, marker_len);
                                    ++i;
                                }
                                if (!sub_text.empty()) {
                                    auto sub_doc = parse_markdown(sub_text);
                                    for (auto& b : sub_doc.blocks) {
                                        items.back().children.push_back(std::move(b));
                                    }
                                }
                                continue;
                            }
                        }
                        break; // end of list
                    }

                    int cur_marker = ordered ? cur_ol : cur_ul;
                    auto content = ll.substr(static_cast<size_t>(cur_marker));

                    // Check for task list checkbox
                    int task = parse_task_checkbox(content);
                    std::optional<bool> checked;
                    if (task >= 0) {
                        checked = (task == 1);
                        content = content.substr(4); // skip "[ ] " or "[x] "
                    }

                    items.push_back(md::ListItem{
                        parse_inlines(content),
                        {},
                        checked
                    });
                    ++i;
                }
                doc.blocks.push_back(md::List{std::move(items), ordered, start_num});
                continue;
            }
        }

        // Table: | col | col |
        if (is_table_row(line)) {
            bool is_table = false;
            if (i + 1 < lines.size()) {
                auto next_line = lines[i + 1];
                if (!next_line.empty() && next_line.back() == '\r')
                    next_line.remove_suffix(1);
                is_table = is_table_separator(next_line);
            }
            if (is_table) {
                flush_paragraph();
                auto header_cells = split_table_cells(line);
                md::TableRow header;
                for (auto& cell : header_cells) {
                    header.cells.push_back(md::TableCell{parse_inlines(cell)});
                }
                i += 2; // skip header + separator

                std::vector<md::TableRow> rows;
                while (i < lines.size()) {
                    auto rl = lines[i];
                    if (!rl.empty() && rl.back() == '\r') rl.remove_suffix(1);
                    if (!is_table_row(rl)) break;
                    auto cells = split_table_cells(rl);
                    md::TableRow row;
                    for (auto& cell : cells) {
                        row.cells.push_back(md::TableCell{parse_inlines(cell)});
                    }
                    rows.push_back(std::move(row));
                    ++i;
                }
                doc.blocks.push_back(md::Table{std::move(header), std::move(rows)});
                continue;
            }
        }

        // Regular paragraph text — single newlines are soft breaks (spaces)
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

// Zed agent panel color palette — One Dark inspired, muted and clean
namespace colors {
    constexpr auto text        = Color::rgb(200, 204, 212);
    constexpr auto heading1    = Color::rgb(224, 226, 232);
    constexpr auto heading2    = Color::rgb(198, 200, 210);
    constexpr auto heading3    = Color::rgb(170, 174, 186);
    constexpr auto heading_dim = Color::rgb(120, 124, 140);
    constexpr auto bold_fg     = Color::rgb(224, 226, 232);
    constexpr auto italic_fg   = Color::rgb(180, 184, 200);
    constexpr auto code_fg     = Color::rgb(209, 154, 102);
    constexpr auto code_bg     = Color::black();
    constexpr auto link_fg     = Color::rgb(97, 175, 239);
    constexpr auto image_fg    = Color::rgb(198, 120, 221);
    constexpr auto strike_fg   = Color::rgb(92, 99, 112);
    constexpr auto quote_bar   = Color::rgb(62, 68, 82);
    constexpr auto quote_text  = Color::rgb(150, 156, 170);
    constexpr auto list_bullet = Color::rgb(92, 99, 112);
    constexpr auto list_num    = Color::rgb(150, 156, 170);
    constexpr auto checkbox_fg = Color::rgb(152, 195, 121);
    constexpr auto checkbox_off= Color::rgb(92, 99, 112);
    constexpr auto code_border = Color::rgb(50, 54, 62);
    constexpr auto code_lang   = Color::rgb(92, 99, 112);
    constexpr auto hrule_fg    = Color::rgb(50, 54, 62);
    constexpr auto footnote_fg = Color::rgb(150, 156, 170);
    constexpr auto table_border= Color::rgb(50, 54, 62);
    constexpr auto table_header= Color::rgb(224, 226, 232);
}

// ============================================================================
// Syntax highlighting for code blocks
// ============================================================================

// One Dark syntax colors (matches Zed's default dark theme)
namespace syntax {
    constexpr auto kw_color      = Color::rgb(198, 120, 221); // purple - keywords
    constexpr auto str_color     = Color::rgb(152, 195, 121); // green - strings
    constexpr auto comment_color = Color::rgb(92, 99, 112);   // gray - comments
    constexpr auto num_color     = Color::rgb(209, 154, 102); // orange - numbers
    constexpr auto type_color    = Color::rgb(229, 192, 123); // yellow - types/caps
    constexpr auto punct_color   = Color::rgb(92, 99, 112);   // gray - punctuation
    constexpr auto fn_color      = Color::rgb(97, 175, 239);  // blue - function calls
    constexpr auto plain_color   = Color::rgb(171, 178, 191); // light - default code
}

// Token kinds for syntax highlighting
enum class TokKind { Plain, Keyword, String, Comment, Number, Type, Punct };

static bool is_keyword(std::string_view word) {
    static constexpr std::string_view kws[] = {
        "if", "else", "for", "while", "return", "class", "struct", "enum",
        "fn", "func", "def", "let", "const", "var", "auto", "void", "int",
        "bool", "string", "import", "from", "export", "use", "pub", "mod",
        "async", "await", "try", "catch", "throw", "new", "delete",
        "true", "false", "null", "nullptr", "None", "self", "this", "super",
        "do", "switch", "case", "break", "continue", "static", "inline",
        "namespace", "template", "typename", "using", "operator", "virtual",
        "override", "final", "explicit", "constexpr", "noexcept", "sizeof",
        "typeof", "type", "interface", "extends", "implements", "package",
        "go", "chan", "select", "defer", "range", "map", "make", "append",
        "lambda", "yield", "with", "pass", "raise", "and", "or", "not", "in",
        "is", "as", "match", "when", "then", "where", "forall",
    };
    for (auto& kw : kws) {
        if (word == kw) return true;
    }
    return false;
}

static bool is_punct_char(char c) {
    return c == '{' || c == '}' || c == '[' || c == ']' ||
           c == '(' || c == ')' || c == '.' || c == ',' ||
           c == ';' || c == ':' || c == '+' || c == '-' ||
           c == '*' || c == '/' || c == '<' || c == '>' ||
           c == '=' || c == '!' || c == '&' || c == '|' ||
           c == '^' || c == '~' || c == '%';
}

// Build a TextElement with syntax-highlighted styled runs for a code block.
static Element highlight_code(const std::string& code, const std::string& /*lang*/) {
    std::string out;
    out.reserve(code.size());
    std::vector<StyledRun> runs;

    size_t i = 0;
    size_t n = code.size();

    auto push_run = [&](size_t start, size_t byte_len, TokKind kind) {
        if (byte_len == 0) return;
        Color c = syntax::plain_color;
        switch (kind) {
            case TokKind::Keyword:  c = syntax::kw_color;      break;
            case TokKind::String:   c = syntax::str_color;     break;
            case TokKind::Comment:  c = syntax::comment_color; break;
            case TokKind::Number:   c = syntax::num_color;     break;
            case TokKind::Type:     c = syntax::type_color;    break;
            case TokKind::Punct:    c = syntax::punct_color;   break;
            case TokKind::Plain:    c = syntax::plain_color;   break;
        }
        Style sty = Style{}.with_fg(c);
        if (kind == TokKind::Comment) sty = sty.with_dim();
        runs.push_back({start, byte_len, sty});
    };

    while (i < n) {
        char ch = code[i];

        // Newline — emit as plain (no run needed, just pass through)
        if (ch == '\n') {
            size_t start = out.size();
            out += '\n';
            push_run(start, 1, TokKind::Plain);
            ++i;
            continue;
        }

        // Line comment: // ... or # ...
        if ((ch == '/' && i + 1 < n && code[i + 1] == '/') ||
            ch == '#') {
            size_t start = out.size();
            size_t j = i;
            while (j < n && code[j] != '\n') ++j;
            out.append(code, i, j - i);
            push_run(start, j - i, TokKind::Comment);
            i = j;
            continue;
        }

        // Block comment: /* ... */
        if (ch == '/' && i + 1 < n && code[i + 1] == '*') {
            size_t start = out.size();
            size_t j = i + 2;
            while (j + 1 < n && !(code[j] == '*' && code[j + 1] == '/')) ++j;
            if (j + 1 < n) j += 2; // consume "*/"
            out.append(code, i, j - i);
            push_run(start, j - i, TokKind::Comment);
            i = j;
            continue;
        }

        // String literal: "...", '...', `...`
        if (ch == '"' || ch == '\'' || ch == '`') {
            char quote = ch;
            size_t start = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == quote) { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            push_run(start, j - i, TokKind::String);
            i = j;
            continue;
        }

        // Number: 0x... or digits[.digits]
        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            (ch == '0' && i + 1 < n && (code[i + 1] == 'x' || code[i + 1] == 'X'))) {
            size_t start = out.size();
            size_t j = i;
            if (j + 1 < n && code[j] == '0' && (code[j + 1] == 'x' || code[j + 1] == 'X')) {
                j += 2;
                while (j < n && std::isxdigit(static_cast<unsigned char>(code[j]))) ++j;
            } else {
                while (j < n && std::isdigit(static_cast<unsigned char>(code[j]))) ++j;
                if (j < n && code[j] == '.' && j + 1 < n &&
                    std::isdigit(static_cast<unsigned char>(code[j + 1]))) {
                    ++j;
                    while (j < n && std::isdigit(static_cast<unsigned char>(code[j]))) ++j;
                }
            }
            out.append(code, i, j - i);
            push_run(start, j - i, TokKind::Number);
            i = j;
            continue;
        }

        // Identifier or keyword or type
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_')) ++j;
            std::string_view word{code.data() + i, j - i};

            // Check for function call: identifier immediately followed by '('
            bool is_fn_call = (j < n && code[j] == '(');

            size_t start = out.size();
            out.append(code, i, j - i);

            TokKind kind = TokKind::Plain;
            if (is_keyword(word)) {
                kind = TokKind::Keyword;
            } else if (is_fn_call) {
                kind = TokKind::Type;  // use fn_color via type slot — reuse fn_color
                // override: use fn_color directly
                push_run(start, j - i, TokKind::Plain);
                // replace last run with fn_color
                runs.back().style = Style{}.with_fg(syntax::fn_color);
                i = j;
                continue;
            } else if (!word.empty() && std::isupper(static_cast<unsigned char>(word[0]))) {
                kind = TokKind::Type;
            }
            push_run(start, j - i, kind);
            i = j;
            continue;
        }

        // Punctuation / operators
        if (is_punct_char(ch)) {
            size_t start = out.size();
            out += ch;
            push_run(start, 1, TokKind::Punct);
            ++i;
            continue;
        }

        // Anything else — plain
        {
            size_t start = out.size();
            out += ch;
            push_run(start, 1, TokKind::Plain);
            ++i;
        }
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = Style{}.with_fg(syntax::plain_color),
        .runs = std::move(runs),
    }};
}

// ============================================================================
// Inline flattening — convert inline AST to a single TextElement with runs
// ============================================================================
// Instead of creating an hstack of separate TextElements (which breaks flex
// layout because each becomes an independent box), flatten all inline spans
// into one TextElement with styled runs.  This lets word wrapping operate on
// the full concatenated text as a single flow.

static void flatten_inline(const md::Inline& span, const Style& inherited,
                           std::string& out, std::vector<StyledRun>& runs) {
    std::visit(overload{
        [&](const md::Text& t) {
            runs.push_back({out.size(), t.content.size(), inherited});
            out += t.content;
        },
        [&](const md::Bold& b) {
            auto sty = inherited.merge(Style{}.with_bold().with_fg(colors::bold_fg));
            for (auto& child : b.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Italic& it) {
            auto sty = inherited.merge(Style{}.with_italic().with_fg(colors::italic_fg));
            for (auto& child : it.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::BoldItalic& bi) {
            auto sty = inherited.merge(Style{}.with_bold().with_italic().with_fg(colors::bold_fg));
            for (auto& child : bi.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Code& c) {
            auto content = " " + c.content + " ";
            auto sty = Style{}.with_fg(colors::code_fg).with_bg(colors::code_bg);
            runs.push_back({out.size(), content.size(), sty});
            out += content;
        },
        [&](const md::Link& l) {
            auto sty = Style{}.with_fg(colors::link_fg).with_underline();
            runs.push_back({out.size(), l.text.size(), sty});
            out += l.text;
        },
        [&](const md::Image& img) {
            std::string display = "\xf0\x9f\x96\xbc " + img.alt;
            auto sty = Style{}.with_fg(colors::image_fg).with_italic();
            runs.push_back({out.size(), display.size(), sty});
            out += display;
        },
        [&](const md::Strike& s) {
            auto sty = inherited.merge(Style{}.with_strikethrough().with_fg(colors::strike_fg));
            for (auto& child : s.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::FootnoteRef& f) {
            auto content = "[" + f.label + "]";
            auto sty = Style{}.with_fg(colors::footnote_fg).with_italic();
            runs.push_back({out.size(), content.size(), sty});
            out += content;
        },
        [&](const md::HardBreak&) {
            runs.push_back({out.size(), 1, inherited});
            out += '\n';
        },
    }, span.inner);
}

// Public API for backward compat — still used for heading prefix rendering
Element md_inline_to_element(const md::Inline& span) {
    std::string content;
    std::vector<StyledRun> runs;
    flatten_inline(span, Style{}.with_fg(colors::text), content, runs);
    if (runs.size() <= 1 && !runs.empty()) {
        return Element{TextElement{.content = std::move(content), .style = runs[0].style}};
    }
    return Element{TextElement{
        .content = std::move(content),
        .style = Style{}.with_fg(colors::text),
        .runs = std::move(runs),
    }};
}

// Measure the display width of a vector of inline spans.
static int measure_inline_width(const std::vector<md::Inline>& spans) {
    std::string content;
    std::vector<StyledRun> runs;
    Style base = Style{}.with_fg(colors::text);
    for (auto& s : spans) {
        flatten_inline(s, base, content, runs);
    }
    return string_width(content);
}

// Build inline spans into a single TextElement with styled runs.
static Element build_inline_row(const std::vector<md::Inline>& spans) {
    if (spans.empty()) return Element{TextElement{}};

    std::string content;
    std::vector<StyledRun> runs;
    Style base = Style{}.with_fg(colors::text);

    for (auto& s : spans) {
        flatten_inline(s, base, content, runs);
    }

    // Optimize: single run → use simple TextElement
    if (runs.size() == 1) {
        return Element{TextElement{
            .content = std::move(content),
            .style = runs[0].style,
        }};
    }

    return Element{TextElement{
        .content = std::move(content),
        .style = base,
        .runs = std::move(runs),
    }};
}

// Forward declaration so render_list can call md_block_to_element.
Element md_block_to_element(const md::Block& block);

// Render an md::List at a given nesting depth (0 = top-level).
static Element render_list(const md::List& l, int depth) {
    std::vector<Element> items;
    items.reserve(l.items.size());
    int num = l.start_num;

    for (auto& item : l.items) {
        std::string prefix;
        Style prefix_style;

        if (item.checked.has_value()) {
            // Zed style: ✓/○ for task lists
            if (*item.checked) {
                prefix = "  \xe2\x9c\x93 ";  // "  ✓ "
                prefix_style = Style{}.with_fg(colors::checkbox_fg);
            } else {
                prefix = "  \xe2\x97\x8b ";  // "  ○ "
                prefix_style = Style{}.with_fg(colors::checkbox_off);
            }
        } else if (l.ordered) {
            prefix = "  " + std::to_string(num++) + ". ";
            prefix_style = Style{}.with_fg(colors::list_num);
        } else if (depth == 0) {
            prefix = "  \xe2\x80\xa2 ";  // "  • " — simple bullet
            prefix_style = Style{}.with_fg(colors::list_bullet);
        } else {
            prefix = "    \xe2\x97\xa6 ";  // "    ◦ " — nested, extra indent
            prefix_style = Style{}.with_fg(colors::list_bullet);
        }

        // Flatten prefix + inline content into single TextElement
        std::string content;
        std::vector<StyledRun> runs;
        Style base = Style{}.with_fg(colors::text);

        runs.push_back({0, prefix.size(), prefix_style});
        content += prefix;
        for (auto& s : item.spans) {
            flatten_inline(s, base, content, runs);
        }

        auto item_elem = Element{TextElement{
            .content = std::move(content),
            .style = base,
            .runs = std::move(runs),
        }};

        if (item.children.empty()) {
            items.push_back(std::move(item_elem));
        } else {
            // Item with sub-content (nested lists, multi-para)
            std::vector<Element> sub;
            sub.reserve(item.children.size() + 1);
            sub.push_back(std::move(item_elem));
            for (auto& child : item.children) {
                // If this child is a list, render it at increased depth
                if (auto* nested = std::get_if<md::List>(&child.inner)) {
                    sub.push_back(render_list(*nested, depth + 1));
                } else {
                    sub.push_back(md_block_to_element(child));
                }
            }
            items.push_back(detail::vstack()(std::move(sub)));
        }
    }
    return detail::vstack()(std::move(items));
}

Element md_block_to_element(const md::Block& block) {
    return std::visit(overload{
        [](const md::Paragraph& p) -> Element {
            return build_inline_row(p.spans);
        },
        [](const md::Heading& h) -> Element {
            Style sty = Style{}.with_bold();
            switch (h.level) {
                case 1: sty = sty.with_fg(colors::heading1); break;
                case 2: sty = sty.with_fg(colors::heading2); break;
                case 3: sty = sty.with_fg(colors::heading3); break;
                default: sty = sty.with_fg(colors::heading3).with_dim(); break;
            }

            // Zed style: clean heading text, no underlines or decorations.
            std::string content;
            std::vector<StyledRun> runs;
            for (auto& s : h.spans) {
                flatten_inline(s, sty, content, runs);
            }
            return Element{TextElement{
                .content = std::move(content),
                .style = sty,
                .runs = std::move(runs),
            }};
        },
        [](const md::CodeBlock& c) -> Element {
            // Zed style: round border, subtle bg, language label top-left
            auto builder = detail::vstack()
                .border(BorderStyle::Round)
                .border_color(colors::code_border)
                .bg(Color::black())
                .padding(0, 1, 0, 1);

            if (!c.lang.empty()) {
                builder = std::move(builder).border_text(
                    " " + c.lang + " ",
                    BorderTextPos::Top,
                    BorderTextAlign::Start);
            }

            return builder(highlight_code(c.content, c.lang));
        },
        [](const md::Blockquote& bq) -> Element {
            // Zed style: thin │ gutter, muted italic text
            std::vector<Element> rows;
            rows.reserve(bq.children.size());
            auto bar_style = Style{}.with_fg(colors::quote_bar);
            auto text_style = Style{}.with_italic().with_fg(colors::quote_text);

            for (auto& child : bq.children) {
                auto child_elem = md_block_to_element(child);
                // Extract text and prefix each visual line with "│ "
                rows.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = "\xe2\x94\x82 ",  // "│ "
                        .style = bar_style,
                    }},
                    std::move(child_elem) | text_style
                ));
            }

            return detail::vstack()(std::move(rows));
        },
        [](const md::List& l) -> Element {
            return render_list(l, 0);
        },
        [](const md::HRule&) -> Element {
            // Zed style: thin, subtle separator
            return Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string rule;
                    rule.reserve(static_cast<size_t>(w) * 3);
                    for (int k = 0; k < w; ++k) rule += "\xe2\x94\x80"; // ─
                    return Element{TextElement{
                        .content = std::move(rule),
                        .style = Style{}.with_fg(colors::hrule_fg),
                    }};
                },
                .layout = {},
            }};
        },
        [](const md::Table& tbl) -> Element {
            int ncols = static_cast<int>(tbl.header.cells.size());
            if (ncols == 0) return Element{TextElement{}};

            // Compute column widths: max of header and all data rows
            std::vector<int> col_widths(static_cast<size_t>(ncols), 0);
            for (int c = 0; c < ncols; ++c) {
                col_widths[static_cast<size_t>(c)] =
                    measure_inline_width(tbl.header.cells[static_cast<size_t>(c)].spans);
                for (auto& row : tbl.rows) {
                    if (static_cast<size_t>(c) < row.cells.size()) {
                        col_widths[static_cast<size_t>(c)] = std::max(
                            col_widths[static_cast<size_t>(c)],
                            measure_inline_width(row.cells[static_cast<size_t>(c)].spans));
                    }
                }
            }

            constexpr int pad = 1; // horizontal padding per side

            // Helper: build a padded cell string of fixed width
            auto pad_cell = [&](const std::string& text, int content_w, int col_w) {
                std::string result;
                int total = col_w + pad * 2;
                result.reserve(static_cast<size_t>(total));
                result.append(static_cast<size_t>(pad), ' ');
                result += text;
                int right = std::max(0, col_w - content_w) + pad;
                result.append(static_cast<size_t>(right), ' ');
                return result;
            };

            // Build header row with fixed-width cells
            std::vector<Element> header_cells;
            header_cells.reserve(static_cast<size_t>(ncols));
            for (int c = 0; c < ncols; ++c) {
                auto& cell = tbl.header.cells[static_cast<size_t>(c)];
                // Flatten to get plain text for padding
                std::string content;
                std::vector<StyledRun> runs;
                auto header_base = Style{}.with_bold().with_fg(colors::table_header);
                for (auto& s : cell.spans) flatten_inline(s, header_base, content, runs);
                int cw = string_width(content);
                int col_w = col_widths[static_cast<size_t>(c)];
                int right_pad = std::max(0, col_w - cw) + pad;
                // Prefix with left pad, append right pad
                std::string padded;
                padded.reserve(static_cast<size_t>(pad) + content.size()
                               + static_cast<size_t>(right_pad));
                padded.append(static_cast<size_t>(pad), ' ');
                padded += content;
                padded.append(static_cast<size_t>(right_pad), ' ');
                // Shift run offsets by left pad
                for (auto& r : runs) r.byte_offset += static_cast<size_t>(pad);
                // Add a run for the padding spaces
                if (!runs.empty()) {
                    runs.insert(runs.begin(), {0, static_cast<size_t>(pad), header_base});
                    runs.push_back({padded.size() - static_cast<size_t>(right_pad),
                                    static_cast<size_t>(right_pad), header_base});
                }
                header_cells.push_back(Element{TextElement{
                    .content = std::move(padded),
                    .style = header_base,
                    .runs = std::move(runs),
                }});
            }

            // Helper: build a horizontal border line (top, mid, bottom)
            // type: 0=top (┌┬┐), 1=mid (├┼┤), 2=bottom (└┴┘)
            auto make_border_line = [&](int type) -> Element {
                const char* left[]  = {"\xe2\x94\x8c", "\xe2\x94\x9c", "\xe2\x94\x94"}; // ┌├└
                const char* mid[]   = {"\xe2\x94\xac", "\xe2\x94\xbc", "\xe2\x94\xb4"}; // ┬┼┴
                const char* right[] = {"\xe2\x94\x90", "\xe2\x94\xa4", "\xe2\x94\x98"}; // ┐┤┘
                std::string line;
                line += left[type];
                for (int c = 0; c < ncols; ++c) {
                    if (c > 0) line += mid[type];
                    int total = col_widths[static_cast<size_t>(c)] + pad * 2;
                    for (int k = 0; k < total; ++k)
                        line += "\xe2\x94\x80"; // "─"
                }
                line += right[type];
                return Element{TextElement{
                    .content = std::move(line),
                    .style = Style{}.with_fg(colors::table_border),
                }};
            };

            // Helper: build a data/header row with │ separators
            auto make_row_with_sep = [&](std::vector<Element>& cells) -> Element {
                std::string content;
                std::vector<StyledRun> runs;
                auto sep_style = Style{}.with_fg(colors::table_border);

                content += "\xe2\x94\x82"; // │
                runs.push_back(StyledRun{0, content.size(), sep_style});

                for (size_t c = 0; c < cells.size(); ++c) {
                    // Extract content and runs from each cell's TextElement
                    auto* te = std::get_if<TextElement>(&cells[c].inner);
                    if (te) {
                        size_t offset = content.size();
                        if (te->runs.empty()) {
                            runs.push_back(StyledRun{offset, te->content.size(), te->style});
                        } else {
                            for (auto& r : te->runs) {
                                runs.push_back(StyledRun{offset + r.byte_offset, r.byte_length, r.style});
                            }
                        }
                        content += te->content;
                    }
                    // Add separator
                    size_t sep_start = content.size();
                    content += "\xe2\x94\x82"; // │
                    runs.push_back(StyledRun{sep_start, 3, sep_style});
                }

                return Element{TextElement{
                    .content = std::move(content),
                    .style = Style{}.with_fg(colors::text),
                    .runs = std::move(runs),
                }};
            };

            std::vector<Element> rows;
            rows.reserve(tbl.rows.size() + 4);

            // Top border ┌──┬──┐
            rows.push_back(make_border_line(0));

            // Header row with │ separators
            rows.push_back(make_row_with_sep(header_cells));

            // Header/data separator ├──┼──┤
            rows.push_back(make_border_line(1));

            // Data rows with │ separators
            for (auto& row : tbl.rows) {
                std::vector<Element> cells;
                cells.reserve(static_cast<size_t>(ncols));
                for (int c = 0; c < ncols; ++c) {
                    if (static_cast<size_t>(c) < row.cells.size()) {
                        auto& cell = row.cells[static_cast<size_t>(c)];
                        std::string content;
                        std::vector<StyledRun> runs;
                        auto base = Style{}.with_fg(colors::text);
                        for (auto& s : cell.spans) flatten_inline(s, base, content, runs);
                        int cw = string_width(content);
                        int col_w = col_widths[static_cast<size_t>(c)];
                        int right_pad_n = std::max(0, col_w - cw) + pad;
                        std::string padded;
                        padded.reserve(static_cast<size_t>(pad) + content.size()
                                       + static_cast<size_t>(right_pad_n));
                        padded.append(static_cast<size_t>(pad), ' ');
                        padded += content;
                        padded.append(static_cast<size_t>(right_pad_n), ' ');
                        for (auto& r : runs) r.byte_offset += static_cast<size_t>(pad);
                        cells.push_back(Element{TextElement{
                            .content = std::move(padded),
                            .style = base,
                            .runs = std::move(runs),
                        }});
                    } else {
                        int total = col_widths[static_cast<size_t>(c)] + pad * 2;
                        std::string empty(static_cast<size_t>(total), ' ');
                        cells.push_back(Element{TextElement{
                            .content = std::move(empty)}});
                    }
                }
                rows.push_back(make_row_with_sep(cells));
            }

            // Bottom border └──┴──┘
            rows.push_back(make_border_line(2));

            return detail::vstack()(std::move(rows));
        },
        [](const md::FootnoteDef& fn) -> Element {
            std::vector<Element> parts;
            parts.reserve(fn.children.size() + 1);
            parts.push_back(Element{TextElement{
                .content = "[" + fn.label + "]:",
                .style = Style{}.with_fg(colors::footnote_fg).with_bold(),
            }});
            for (auto& child : fn.children) {
                parts.push_back(detail::hstack()(
                    Element{TextElement{.content = "  "}},
                    md_block_to_element(child)
                ));
            }
            return detail::vstack()(std::move(parts));
        },
    }, block.inner);
}

Element markdown(std::string_view source) {
    auto doc = parse_markdown(source);
    if (doc.blocks.empty()) return Element{TextElement{""}};

    if (doc.blocks.size() == 1)
        return detail::vstack().padding(0, 0, 0, 2)(md_block_to_element(doc.blocks[0]));

    std::vector<Element> blocks;
    blocks.reserve(doc.blocks.size());
    for (auto& block : doc.blocks)
        blocks.push_back(md_block_to_element(block));

    return detail::vstack().gap(1).padding(0, 0, 0, 2)(std::move(blocks));
}

// ============================================================================
// StreamingMarkdown — progressive per-block rendering
// ============================================================================

size_t StreamingMarkdown::find_block_boundary() const noexcept {
    bool in_fence = in_code_fence_;
    size_t last_boundary = committed_;

    size_t i = committed_;
    while (i < source_.size()) {
        // Check for code fence toggle (``` or ~~~)
        if (i + 3 <= source_.size() &&
            ((source_[i] == '`' && source_[i+1] == '`' && source_[i+2] == '`') ||
             (source_[i] == '~' && source_[i+1] == '~' && source_[i+2] == '~'))) {
            size_t eol = source_.find('\n', i);
            if (eol == std::string::npos) break;
            in_fence = !in_fence;
            i = eol + 1;
            if (!in_fence) {
                last_boundary = i;
            }
            continue;
        }

        // Check for blank line (block separator) outside code fences
        if (!in_fence && source_[i] == '\n') {
            size_t next = i + 1;
            if (next < source_.size() && source_[next] == '\n') {
                last_boundary = next + 1;
                i = next + 1;
                continue;
            }
            // Single newline + block-level marker = boundary
            if (next < source_.size()) {
                char c = source_[next];
                if (c == '#' || c == '>' || c == '-' || c == '*' || c == '+' || c == '|') {
                    last_boundary = next;
                }
            }
        }

        size_t eol = source_.find('\n', i);
        if (eol == std::string::npos) break;
        i = eol + 1;
    }

    return last_boundary;
}

void StreamingMarkdown::set_content(std::string_view content) {
    if (content.size() >= source_.size() &&
        content.substr(0, source_.size()) == source_) {
        if (content.size() > source_.size()) {
            source_ = std::string{content};
        }
    } else {
        clear();
        source_ = std::string{content};
    }

    size_t boundary = find_block_boundary();
    if (boundary > committed_) {
        auto new_text = std::string_view{source_}.substr(committed_, boundary - committed_);
        auto doc = parse_markdown(new_text);
        for (auto& block : doc.blocks) {
            blocks_.push_back(md_block_to_element(block));
        }
        for (size_t j = committed_; j < boundary; ++j) {
            if (j + 3 <= boundary &&
                ((source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') ||
                 (source_[j] == '~' && source_[j+1] == '~' && source_[j+2] == '~'))) {
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
                ((source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') ||
                 (source_[j] == '~' && source_[j+1] == '~' && source_[j+2] == '~'))) {
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
    std::string_view tail;
    if (committed_ < source_.size()) {
        tail = std::string_view{source_}.substr(committed_);
    }

    bool has_tail = !tail.empty();
    size_t total = blocks_.size() + (has_tail ? 1 : 0);

    if (total == 0) return Element{TextElement{""}};

    if (total == 1 && !has_tail)
        return detail::vstack().padding(0, 0, 0, 2)(blocks_[0]);

    if (total == 1 && blocks_.empty()) {
        return markdown(tail);
    }

    std::vector<Element> all;
    all.reserve(total);
    for (auto& b : blocks_) all.push_back(b);

    if (has_tail) {
        auto tail_doc = parse_markdown(tail);
        for (auto& block : tail_doc.blocks) {
            all.push_back(md_block_to_element(block));
        }
    }

    return detail::vstack().gap(1).padding(0, 0, 0, 2)(std::move(all));
}

} // namespace maya
