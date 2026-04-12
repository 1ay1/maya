#include "maya/widget/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
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

// ── Compile-time lookup tables ──────────────────────────────────────────────

// Helper: build a 256-byte boolean lookup table at compile time.
struct CharTable {
    bool v[256]{};
    constexpr bool operator[](unsigned char c) const noexcept { return v[c]; }
};

template <unsigned char... Cs>
consteval CharTable make_table() {
    CharTable t{};
    ((t.v[Cs] = true), ...);
    return t;
}

// Escapable CommonMark characters.
static constexpr auto kEscapable = make_table<
    '\\', '`', '*', '_', '{', '}', '[', ']', '(', ')', '#', '+',
    '-', '.', '!', '|', '~', '<', '>', '"', '\'', '^'>();

// Characters that break the inline plain-text batch scanner.
static constexpr auto kInlineSpecial = make_table<
    '`', '*', '_', '~', '[', '!', '\\', '<', '$'>();

// Punctuation characters in syntax highlighting.
static constexpr auto kPunctChar = make_table<
    '{', '}', '[', ']', '(', ')', '.', ',', ';', ':',
    '<', '>', '?', '~', '%', '@', '\\'>();

// Operator characters in syntax highlighting.
static constexpr auto kOpChar = make_table<
    '+', '-', '*', '/', '=', '!', '&', '|', '^'>();

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

inline bool is_escapable(char c) {
    return kEscapable[static_cast<unsigned char>(c)];
}

// ============================================================================
// Inline parser — single-pass, stack-based delimiter matching
// ============================================================================

// Find the closing delimiter (linear scan — called only when open found).
// Respects backslash escapes.  max_dist caps how far we scan to keep
// pathological input (many unmatched delimiters) O(n) instead of O(n²).
size_t find_closing(std::string_view text, std::string_view delim,
                    size_t start, size_t max_dist = 2000) {
    size_t limit = std::min(text.size(), start + max_dist);
    // Specialize for common 1-byte and 2-byte delimiters to avoid
    // substr construction + comparison overhead in the inner loop.
    if (delim.size() == 1) [[likely]] {
        char d = delim[0];
        for (size_t i = start; i < limit; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) { ++i; continue; }
            if (text[i] == d) return i;
        }
        return std::string_view::npos;
    }
    if (delim.size() == 2) {
        char d0 = delim[0], d1 = delim[1];
        for (size_t i = start; i + 1 < limit; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) { ++i; continue; }
            if (text[i] == d0 && text[i + 1] == d1) return i;
        }
        return std::string_view::npos;
    }
    for (size_t i = start; i + delim.size() <= limit; ++i) {
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

        // Math: $$...$$ (display) or $...$ (inline) — render as code to
        // prevent delimiters inside math from being parsed as emphasis.
        if (text[i] == '$') {
            // Display math $$...$$
            if (i + 1 < text.size() && text[i + 1] == '$') {
                size_t end = text.find("$$", i + 2);
                if (end != std::string_view::npos) {
                    auto content = text.substr(i + 2, end - i - 2);
                    result.push_back(md::Code{std::string{content}});
                    i = end + 2;
                    continue;
                }
            }
            // Inline math $...$  — require non-space flanking to avoid
            // matching currency like "$5 and $10".
            if (i + 1 < text.size() && text[i + 1] != ' ' && text[i + 1] != '$') {
                size_t end = text.find('$', i + 1);
                if (end != std::string_view::npos && end > i + 1 &&
                    text[end - 1] != ' ') {
                    auto content = text.substr(i + 1, end - i - 1);
                    result.push_back(md::Code{std::string{content}});
                    i = end + 1;
                    continue;
                }
            }
            push_text(result, "$");
            ++i;
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
                // Bounded scan to avoid O(n²) on many unmatched delimiters
                size_t scan_limit = std::min(text.size(), i + 1 + 2000);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < scan_limit; ++s) {
                    if (text[s] == delim_ch) { end = s; break; }
                }
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

        // Image: ![alt](url) — bounded bracket/paren search
        if (text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[') {
            size_t bracket_limit = std::min(text.size(), i + 2 + 2000);
            size_t close_bracket = std::string_view::npos;
            for (size_t s = i + 2; s < bracket_limit; ++s) {
                if (text[s] == ']') { close_bracket = s; break; }
            }
            if (close_bracket != std::string_view::npos &&
                close_bracket + 1 < text.size() &&
                text[close_bracket + 1] == '(') {
                size_t paren_limit = std::min(text.size(), close_bracket + 2 + 2000);
                size_t close_paren = std::string_view::npos;
                for (size_t s = close_bracket + 2; s < paren_limit; ++s) {
                    if (text[s] == ')') { close_paren = s; break; }
                }
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

        // Link: [text](url) — bounded bracket/paren search
        if (text[i] == '[') {
            // Footnote reference: [^label]
            if (i + 1 < text.size() && text[i + 1] == '^') {
                size_t fn_limit = std::min(text.size(), i + 2 + 200);
                size_t close = std::string_view::npos;
                for (size_t s = i + 2; s < fn_limit; ++s) {
                    if (text[s] == ']') { close = s; break; }
                }
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

            size_t bracket_limit = std::min(text.size(), i + 1 + 2000);
            size_t close_bracket = std::string_view::npos;
            for (size_t s = i + 1; s < bracket_limit; ++s) {
                if (text[s] == ']') { close_bracket = s; break; }
            }
            if (close_bracket != std::string_view::npos &&
                close_bracket + 1 < text.size() &&
                text[close_bracket + 1] == '(') {
                size_t paren_limit = std::min(text.size(), close_bracket + 2 + 2000);
                size_t close_paren = std::string_view::npos;
                for (size_t s = close_bracket + 2; s < paren_limit; ++s) {
                    if (text[s] == ')') { close_paren = s; break; }
                }
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
               !kInlineSpecial[static_cast<unsigned char>(text[i])]) {
            // Also break on trailing spaces before newline (hard break detection)
            if (text[i] == ' ' && i + 2 < text.size()) {
                size_t j = i;
                while (j < text.size() && text[j] == ' ') ++j;
                if (j < text.size() && text[j] == '\n' && (j - i) >= 2) break;
                // Skip past all checked spaces to avoid O(n²) re-scanning
                i = j;
                continue;
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

// SIMD-accelerated newline search via memchr (outside anonymous namespace
// so highlight_diff / highlight_code can reference it).
static inline size_t find_eol(const char* data, size_t start, size_t end) noexcept {
    if (start >= end) return end;
    auto* p = static_cast<const char*>(
        std::memchr(data + start, '\n', end - start));
    return p ? static_cast<size_t>(p - data) : end;
}

// ============================================================================
// Block parser — line-oriented markdown parsing
// ============================================================================

static constexpr int kMaxRecursionDepth = 8;

static md::Document parse_markdown_impl(std::string_view source, int depth) {
    md::Document doc;

    // Guard against pathological nesting (deeply nested blockquotes, lists,
    // footnotes).  Treat remaining text as a plain paragraph.
    if (depth > kMaxRecursionDepth) {
        auto trimmed = trim(source);
        if (!trimmed.empty())
            doc.blocks.push_back(md::Paragraph{parse_inlines(trimmed)});
        return doc;
    }

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

        // Cache trimmed line — trim() is called 3-7× per iteration
        // in the worst case (blank, $$, hrule, setext, blockquote, footnote…).
        auto trimmed = trim(line);

        // Blank line
        if (trimmed.empty()) {
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

        // Display math block: $$ on its own line
        if (trimmed == "$$") {
            flush_paragraph();
            std::string math;
            ++i;
            while (i < lines.size()) {
                auto ml = lines[i];
                if (!ml.empty() && ml.back() == '\r') ml.remove_suffix(1);
                if (trim(ml) == "$$") { ++i; break; }
                if (!math.empty()) math += '\n';
                math += ml;
                ++i;
            }
            doc.blocks.push_back(md::CodeBlock{std::move(math), "math"});
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
            auto t = trimmed;
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
            auto t = trimmed;
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
        if (!trimmed.empty() && trimmed[0] == '>') {
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
            auto inner = parse_markdown_impl(bq_text, depth + 1);
            doc.blocks.push_back(md::Blockquote{std::move(inner.blocks)});
            continue;
        }

        // Footnote definition: [^label]: content
        if (starts_with(trimmed, "[^")) {
            auto t = trimmed;
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
                auto inner = parse_markdown_impl(fn_text, depth + 1);
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
                                    auto sub_doc = parse_markdown_impl(sub_text, depth + 1);
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

md::Document parse_markdown(std::string_view source) {
    return parse_markdown_impl(source, 0);
}

// ============================================================================
// AST to Element conversion — polished terminal rendering
// ============================================================================

// Terminal-adaptive color palette — uses named ANSI colors so the rendering
// automatically matches whatever terminal theme the user has configured
// (Catppuccin, Dracula, Solarized, One Dark, Gruvbox, etc.)
namespace colors {
    constexpr auto text        = Color::white();
    constexpr auto heading1    = Color::bright_white();
    constexpr auto heading2    = Color::bright_white();
    constexpr auto heading3    = Color::white();
    constexpr auto heading_dim = Color::bright_black();
    constexpr auto bold_fg     = Color::bright_white();
    constexpr auto italic_fg   = Color::white();
    constexpr auto code_fg     = Color::bright_yellow();
    constexpr auto code_bg     = Color::black();
    constexpr auto link_fg     = Color::bright_blue();
    constexpr auto image_fg    = Color::bright_magenta();
    constexpr auto strike_fg   = Color::bright_black();
    constexpr auto quote_bar   = Color::bright_black();
    constexpr auto quote_text  = Color::white();
    constexpr auto list_bullet = Color::bright_black();
    constexpr auto list_num    = Color::white();
    constexpr auto checkbox_fg = Color::bright_green();
    constexpr auto checkbox_off= Color::bright_black();
    constexpr auto code_border = Color::bright_black();
    constexpr auto code_lang   = Color::bright_black();
    constexpr auto hrule_fg    = Color::bright_black();
    constexpr auto footnote_fg = Color::bright_black();
    constexpr auto table_border= Color::bright_black();
    constexpr auto table_header= Color::bright_white();
}

// ============================================================================
// Language-aware syntax highlighting for code blocks
// ============================================================================
// Uses only terminal named ANSI colors so highlighting adapts to the user's
// terminal theme (Catppuccin, Dracula, Solarized, One Dark, Gruvbox, etc.)

namespace syntax {
    // Static const: constructed once, returned by reference — avoids
    // rebuilding Style objects on every token emission.
    inline const Style& kw()       { static const Style s = Style{}.with_fg(Color::magenta()); return s; }
    inline const Style& ctrl()     { static const Style s = Style{}.with_fg(Color::magenta()); return s; }
    inline const Style& type()     { static const Style s = Style{}.with_fg(Color::cyan()); return s; }
    inline const Style& fn()       { static const Style s = Style{}.with_fg(Color::blue()); return s; }
    inline const Style& str()      { static const Style s = Style{}.with_fg(Color::green()); return s; }
    inline const Style& num()      { static const Style s = Style{}.with_fg(Color::bright_yellow()); return s; }
    inline const Style& comment()  { static const Style s = Style{}.with_fg(Color::bright_black()).with_italic(); return s; }
    inline const Style& constant() { static const Style s = Style{}.with_fg(Color::bright_yellow()); return s; }
    inline const Style& preproc()  { static const Style s = Style{}.with_fg(Color::yellow()); return s; }
    inline const Style& attr()     { static const Style s = Style{}.with_fg(Color::yellow()); return s; }
    inline const Style& op()       { static const Style s = Style{}.with_fg(Color::red()); return s; }
    inline const Style& punct()    { static const Style s = Style{}.with_fg(Color::bright_black()); return s; }
    inline const Style& plain()    { static const Style s = Style{}.with_fg(Color::white()); return s; }
    inline const Style& shellvar() { static const Style s = Style{}.with_fg(Color::bright_cyan()); return s; }

    // Diff highlighting
    inline const Style& diff_add()  { static const Style s = Style{}.with_fg(Color::green()); return s; }
    inline const Style& diff_del()  { static const Style s = Style{}.with_fg(Color::red()); return s; }
    inline const Style& diff_hunk() { static const Style s = Style{}.with_fg(Color::cyan()); return s; }
    inline const Style& diff_meta() { static const Style s = Style{}.with_fg(Color::bright_black()).with_bold(); return s; }
}

// ── Language identification ──────────────────────────────────────────────────

enum class LangId {
    Unknown,
    C, Cpp, Python, Rust, JavaScript, TypeScript, Go, Java, Kotlin, Swift,
    Ruby, Shell, Fish, SQL, HTML, XML, CSS, SCSS,
    JSON, YAML, TOML, Lua, Zig, Haskell, Elixir, Erlang, PHP, Perl, R,
    Diff, Makefile, CMake, Dockerfile, Markdown,
};

static LangId detect_lang(std::string_view tag) {
    // Normalize: lowercase — use stack buffer to avoid heap allocation
    // (language tags are always short, typically < 16 chars).
    char buf[32];
    size_t len = std::min(tag.size(), sizeof(buf) - 1);
    for (size_t k = 0; k < len; ++k)
        buf[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[k])));
    buf[len] = '\0';
    std::string_view lower{buf, len};

    if (lower == "c" || lower == "h")                return LangId::C;
    if (lower == "cpp" || lower == "c++" ||
        lower == "cxx" || lower == "cc" ||
        lower == "hpp" || lower == "hxx")            return LangId::Cpp;
    if (lower == "python" || lower == "py")          return LangId::Python;
    if (lower == "rust" || lower == "rs")            return LangId::Rust;
    if (lower == "javascript" || lower == "js" ||
        lower == "jsx" || lower == "mjs" ||
        lower == "cjs")                              return LangId::JavaScript;
    if (lower == "typescript" || lower == "ts" ||
        lower == "tsx")                              return LangId::TypeScript;
    if (lower == "go" || lower == "golang")          return LangId::Go;
    if (lower == "java")                             return LangId::Java;
    if (lower == "kotlin" || lower == "kt" ||
        lower == "kts")                              return LangId::Kotlin;
    if (lower == "swift")                            return LangId::Swift;
    if (lower == "ruby" || lower == "rb")            return LangId::Ruby;
    if (lower == "bash" || lower == "sh" ||
        lower == "shell" || lower == "zsh")          return LangId::Shell;
    if (lower == "fish")                             return LangId::Fish;
    if (lower == "sql" || lower == "mysql" ||
        lower == "postgresql" || lower == "sqlite")  return LangId::SQL;
    if (lower == "html" || lower == "htm")           return LangId::HTML;
    if (lower == "xml" || lower == "svg")            return LangId::XML;
    if (lower == "css")                              return LangId::CSS;
    if (lower == "scss" || lower == "sass" ||
        lower == "less")                             return LangId::SCSS;
    if (lower == "json" || lower == "jsonc")         return LangId::JSON;
    if (lower == "yaml" || lower == "yml")           return LangId::YAML;
    if (lower == "toml")                             return LangId::TOML;
    if (lower == "lua")                              return LangId::Lua;
    if (lower == "zig")                              return LangId::Zig;
    if (lower == "haskell" || lower == "hs")         return LangId::Haskell;
    if (lower == "elixir" || lower == "ex" ||
        lower == "exs")                              return LangId::Elixir;
    if (lower == "erlang" || lower == "erl")         return LangId::Erlang;
    if (lower == "php")                              return LangId::PHP;
    if (lower == "perl" || lower == "pl")            return LangId::Perl;
    if (lower == "r")                                return LangId::R;
    if (lower == "diff" || lower == "patch")         return LangId::Diff;
    if (lower == "makefile" || lower == "make")      return LangId::Makefile;
    if (lower == "cmake")                            return LangId::CMake;
    if (lower == "dockerfile" || lower == "docker")  return LangId::Dockerfile;
    if (lower == "markdown" || lower == "md")        return LangId::Markdown;
    return LangId::Unknown;
}

// ── Per-language keyword tables ──────────────────────────────────────────────

static bool in_list(std::string_view word, std::initializer_list<std::string_view> list) {
    for (auto& k : list) if (word == k) return true;
    return false;
}

struct WordClass { bool keyword; bool type; bool constant; };

static WordClass classify_word(std::string_view word, LangId lang) {
    // Constants — universal
    if (in_list(word, {"true", "false", "null", "nullptr", "None", "nil",
                       "True", "False", "NULL", "NaN", "Infinity",
                       "undefined", "NUL", "YES", "NO"}))
        return {false, false, true};

    switch (lang) {
    case LangId::C:
    case LangId::Cpp:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "goto", "default",
            "struct", "enum", "union", "typedef", "class", "namespace",
            "template", "typename", "using", "static", "extern", "inline",
            "const", "constexpr", "consteval", "constinit", "volatile",
            "mutable", "register", "thread_local",
            "virtual", "override", "final", "explicit", "noexcept",
            "public", "private", "protected", "friend",
            "new", "delete", "operator", "sizeof", "alignof", "decltype",
            "static_assert", "static_cast", "dynamic_cast", "reinterpret_cast",
            "const_cast", "typeid", "throw", "try", "catch",
            "concept", "requires", "co_await", "co_yield", "co_return",
            "export", "import", "module",
            "auto", "void",
            "#include", "#define", "#ifdef", "#ifndef", "#endif", "#pragma",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "char", "float", "double", "long", "short", "unsigned",
            "signed", "bool", "size_t", "uint8_t", "uint16_t", "uint32_t",
            "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
            "string", "string_view", "vector", "map", "set", "array",
            "optional", "variant", "pair", "tuple", "span", "expected",
            "unique_ptr", "shared_ptr", "weak_ptr",
            "wchar_t", "char8_t", "char16_t", "char32_t", "ptrdiff_t",
        })) return {false, true, false};
        break;

    case LangId::Python:
        if (in_list(word, {
            "if", "elif", "else", "for", "while", "break", "continue",
            "return", "yield", "pass", "raise", "try", "except", "finally",
            "with", "as", "assert", "del",
            "def", "class", "lambda", "async", "await",
            "import", "from", "global", "nonlocal",
            "and", "or", "not", "in", "is",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "float", "str", "bool", "list", "dict", "tuple", "set",
            "bytes", "bytearray", "complex", "frozenset", "type", "object",
            "range", "enumerate", "zip", "map", "filter",
            "Exception", "ValueError", "TypeError", "KeyError", "IndexError",
            "RuntimeError", "StopIteration", "AttributeError", "ImportError",
            "OSError", "IOError", "FileNotFoundError",
        })) return {false, true, false};
        if (in_list(word, {"self", "cls", "super"}))
            return {true, false, false};
        break;

    case LangId::Rust:
        if (in_list(word, {
            "if", "else", "for", "while", "loop", "break", "continue",
            "return", "match", "as",
            "fn", "struct", "enum", "impl", "trait", "type", "where",
            "let", "mut", "const", "static", "ref", "move",
            "pub", "mod", "use", "crate", "super", "self", "Self",
            "async", "await", "unsafe", "extern", "dyn",
        })) return {true, false, false};
        if (in_list(word, {
            "i8", "i16", "i32", "i64", "i128", "isize",
            "u8", "u16", "u32", "u64", "u128", "usize",
            "f32", "f64", "bool", "char", "str",
            "String", "Vec", "Box", "Rc", "Arc", "Cell", "RefCell",
            "Option", "Result", "Ok", "Err", "Some",
            "HashMap", "HashSet", "BTreeMap", "BTreeSet",
            "Iterator", "IntoIterator", "From", "Into",
            "Display", "Debug", "Clone", "Copy", "Send", "Sync",
            "Default", "PartialEq", "Eq", "PartialOrd", "Ord", "Hash",
        })) return {false, true, false};
        break;

    case LangId::JavaScript:
    case LangId::TypeScript:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "throw", "try", "catch", "finally",
            "default", "in", "of", "typeof", "instanceof", "void", "delete",
            "function", "class", "extends", "new", "this", "super",
            "const", "let", "var", "async", "await", "yield",
            "import", "export", "from", "as", "default",
            "with", "debugger",
        })) return {true, false, false};
        if (lang == LangId::TypeScript && in_list(word, {
            "type", "interface", "enum", "namespace", "declare", "abstract",
            "implements", "readonly", "keyof", "infer", "is", "asserts",
            "override", "satisfies",
        })) return {true, false, false};
        if (in_list(word, {
            "string", "number", "boolean", "object", "symbol", "bigint",
            "any", "unknown", "never", "void",
            "Array", "Map", "Set", "Promise", "Date", "RegExp", "Error",
            "Object", "Function", "Symbol", "WeakMap", "WeakSet",
            "Record", "Partial", "Required", "Readonly", "Pick", "Omit",
        })) return {false, true, false};
        break;

    case LangId::Go:
        if (in_list(word, {
            "if", "else", "for", "switch", "case", "break", "continue",
            "return", "goto", "default", "fallthrough", "select",
            "func", "type", "struct", "interface", "map", "chan",
            "var", "const", "package", "import",
            "go", "defer", "range",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
            "float32", "float64", "complex64", "complex128",
            "bool", "byte", "rune", "string", "error",
            "any", "comparable",
        })) return {false, true, false};
        if (in_list(word, {"make", "append", "len", "cap", "copy", "close",
                           "new", "delete", "panic", "recover", "print", "println",
                           "iota"}))
            return {false, false, true};
        break;

    case LangId::Java:
    case LangId::Kotlin:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "throw", "try", "catch", "finally",
            "default", "instanceof", "new", "this", "super",
            "class", "interface", "enum", "extends", "implements",
            "abstract", "final", "static", "synchronized", "volatile",
            "transient", "native", "strictfp",
            "public", "private", "protected", "package", "import",
            "assert", "throws", "void",
        })) return {true, false, false};
        if (lang == LangId::Kotlin && in_list(word, {
            "fun", "val", "var", "when", "is", "as", "in", "out",
            "object", "companion", "data", "sealed", "inline", "reified",
            "suspend", "override", "open", "internal", "lateinit",
            "by", "constructor", "init", "typealias",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "long", "short", "byte", "float", "double", "char",
            "boolean", "String", "Integer", "Long", "Double", "Float",
            "Boolean", "Character", "Object", "Void",
            "List", "Map", "Set", "Array", "Collection", "Iterator",
            "Optional", "Stream", "Comparable", "Iterable",
        })) return {false, true, false};
        break;

    case LangId::Ruby:
        if (in_list(word, {
            "if", "elsif", "else", "unless", "while", "until", "for",
            "do", "end", "begin", "rescue", "ensure", "raise", "retry",
            "return", "break", "next", "redo", "yield",
            "def", "class", "module", "include", "extend", "require",
            "require_relative", "attr_reader", "attr_writer", "attr_accessor",
            "self", "super", "then", "when", "case", "in", "and", "or", "not",
            "defined?", "alias", "undef", "private", "protected", "public",
            "lambda", "proc", "block_given?",
        })) return {true, false, false};
        break;

    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:
        if (in_list(word, {
            "if", "then", "else", "elif", "fi", "for", "while", "until",
            "do", "done", "case", "esac", "in", "function", "return",
            "local", "export", "unset", "readonly", "declare", "typeset",
            "source", "eval", "exec", "set", "shift", "trap",
            "echo", "printf", "read", "test", "exit",
            // Dockerfile
            "FROM", "RUN", "CMD", "ENTRYPOINT", "COPY", "ADD", "WORKDIR",
            "ENV", "ARG", "EXPOSE", "VOLUME", "USER", "LABEL", "ONBUILD",
            "HEALTHCHECK", "SHELL", "STOPSIGNAL",
        })) return {true, false, false};
        break;

    case LangId::SQL:
        if (in_list(word, {
            "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE",
            "SET", "DELETE", "CREATE", "ALTER", "DROP", "TABLE", "INDEX",
            "VIEW", "DATABASE", "SCHEMA", "JOIN", "LEFT", "RIGHT", "INNER",
            "OUTER", "FULL", "CROSS", "ON", "AS", "AND", "OR", "NOT", "IN",
            "IS", "LIKE", "BETWEEN", "EXISTS", "HAVING", "GROUP", "BY",
            "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "UNION", "ALL",
            "DISTINCT", "CASE", "WHEN", "THEN", "ELSE", "END", "IF",
            "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION", "GRANT", "REVOKE",
            "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "CONSTRAINT",
            "UNIQUE", "CHECK", "DEFAULT", "CASCADE", "RESTRICT",
            // Also match lowercase
            "select", "from", "where", "insert", "into", "values", "update",
            "set", "delete", "create", "alter", "drop", "table", "index",
            "view", "database", "schema", "join", "left", "right", "inner",
            "outer", "full", "cross", "on", "as", "and", "or", "not", "in",
            "is", "like", "between", "exists", "having", "group", "by",
            "order", "asc", "desc", "limit", "offset", "union", "all",
            "distinct", "case", "when", "then", "else", "end", "if",
            "begin", "commit", "rollback", "transaction", "grant", "revoke",
            "primary", "key", "foreign", "references", "constraint",
            "unique", "check", "default", "cascade", "restrict",
        })) return {true, false, false};
        if (in_list(word, {
            "INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT",
            "VARCHAR", "CHAR", "TEXT", "BLOB", "BOOLEAN", "BOOL",
            "FLOAT", "DOUBLE", "DECIMAL", "NUMERIC", "REAL",
            "DATE", "TIME", "TIMESTAMP", "DATETIME",
            "SERIAL", "BIGSERIAL", "UUID",
            "int", "integer", "bigint", "smallint", "tinyint",
            "varchar", "char", "text", "blob", "boolean", "bool",
            "float", "double", "decimal", "numeric", "real",
            "date", "time", "timestamp", "datetime",
            "serial", "bigserial", "uuid",
        })) return {false, true, false};
        break;

    case LangId::Lua:
        if (in_list(word, {
            "if", "then", "else", "elseif", "end", "for", "while", "do",
            "repeat", "until", "break", "return", "goto",
            "function", "local", "in", "and", "or", "not",
        })) return {true, false, false};
        break;

    case LangId::Zig:
        if (in_list(word, {
            "if", "else", "for", "while", "break", "continue", "return",
            "switch", "orelse", "catch", "unreachable",
            "fn", "pub", "const", "var", "struct", "enum", "union",
            "error", "test", "comptime", "inline", "extern", "export",
            "threadlocal", "defer", "errdefer", "nosuspend",
            "try", "async", "await", "suspend", "resume",
            "align", "allowzero", "volatile", "linksection",
        })) return {true, false, false};
        if (in_list(word, {
            "u8", "u16", "u32", "u64", "u128", "usize",
            "i8", "i16", "i32", "i64", "i128", "isize",
            "f16", "f32", "f64", "f128", "bool", "void", "noreturn",
            "anyerror", "anyframe", "anytype", "anyopaque", "type",
            "comptime_int", "comptime_float",
        })) return {false, true, false};
        break;

    case LangId::Swift:
        if (in_list(word, {
            "if", "else", "for", "while", "repeat", "switch", "case",
            "break", "continue", "return", "throw", "guard", "defer",
            "do", "try", "catch", "where", "in", "as", "is",
            "func", "class", "struct", "enum", "protocol", "extension",
            "typealias", "associatedtype",
            "let", "var", "static", "lazy", "override", "mutating",
            "public", "private", "internal", "fileprivate", "open",
            "import", "init", "deinit", "subscript", "operator",
            "async", "await", "actor",
            "self", "Self", "super",
        })) return {true, false, false};
        if (in_list(word, {
            "Int", "Int8", "Int16", "Int32", "Int64",
            "UInt", "UInt8", "UInt16", "UInt32", "UInt64",
            "Float", "Double", "Bool", "String", "Character",
            "Array", "Dictionary", "Set", "Optional",
            "Any", "AnyObject", "Void", "Never",
        })) return {false, true, false};
        break;

    case LangId::Haskell:
        if (in_list(word, {
            "if", "then", "else", "case", "of", "let", "in", "where",
            "do", "module", "import", "data", "type", "newtype", "class",
            "instance", "deriving", "default", "forall", "infixl", "infixr",
            "infix", "qualified", "as", "hiding",
        })) return {true, false, false};
        if (in_list(word, {
            "Int", "Integer", "Float", "Double", "Char", "String", "Bool",
            "IO", "Maybe", "Either", "Monad", "Functor", "Applicative",
            "Show", "Read", "Eq", "Ord", "Num", "Enum", "Bounded",
        })) return {false, true, false};
        break;

    case LangId::Elixir:
    case LangId::Erlang:
        if (in_list(word, {
            "if", "else", "do", "end", "case", "cond", "when", "with",
            "for", "unless", "fn", "def", "defp", "defmodule", "defstruct",
            "defimpl", "defprotocol", "defmacro", "defguard",
            "import", "require", "use", "alias", "raise", "rescue",
            "try", "catch", "after", "receive", "send", "spawn",
            "and", "or", "not", "in",
        })) return {true, false, false};
        break;

    case LangId::PHP:
        if (in_list(word, {
            "if", "else", "elseif", "for", "foreach", "while", "do",
            "switch", "case", "break", "continue", "return", "throw",
            "try", "catch", "finally", "default",
            "function", "class", "interface", "trait", "extends", "implements",
            "abstract", "final", "static", "public", "private", "protected",
            "new", "instanceof", "as", "use", "namespace", "echo", "print",
            "include", "require", "include_once", "require_once",
            "yield", "fn", "match", "enum", "readonly",
        })) return {true, false, false};
        break;

    case LangId::CSS:
    case LangId::SCSS:
    case LangId::HTML:
    case LangId::XML:
    case LangId::JSON:
    case LangId::YAML:
    case LangId::TOML:
    case LangId::Perl:
    case LangId::R:
    case LangId::CMake:
    case LangId::Markdown:
    case LangId::Unknown:
        break;
    }

    return {false, false, false};
}

// ── Comment style per language ───────────────────────────────────────────────

struct CommentStyle {
    const char* line;         // "//" or "#" or "--" or nullptr
    const char* block_open;   // "/*" or "{-" or nullptr
    const char* block_close;  // "*/" or "-}" or nullptr
    bool hash_comment;        // '#' as line comment (separate from "//" because
                              // '#' can also be preprocessor in C/C++)
};

static CommentStyle comment_style_for(LangId lang) {
    switch (lang) {
    case LangId::C:
    case LangId::Cpp:          return {"//", "/*", "*/", false};
    case LangId::Python:       return {nullptr, nullptr, nullptr, true};
    case LangId::Rust:         return {"//", "/*", "*/", false};
    case LangId::JavaScript:
    case LangId::TypeScript:   return {"//", "/*", "*/", false};
    case LangId::Go:           return {"//", "/*", "*/", false};
    case LangId::Java:
    case LangId::Kotlin:       return {"//", "/*", "*/", false};
    case LangId::Swift:        return {"//", "/*", "*/", false};
    case LangId::Zig:          return {"//", nullptr, nullptr, false};
    case LangId::Lua:          return {"--", "--[[", "]]", false};
    case LangId::Haskell:      return {"--", "{-", "-}", false};
    case LangId::SQL:          return {"--", "/*", "*/", false};
    case LangId::Ruby:         return {nullptr, "=begin", "=end", true};
    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:
    case LangId::YAML:
    case LangId::TOML:
    case LangId::R:
    case LangId::CMake:
    case LangId::Elixir:
    case LangId::Erlang:       return {nullptr, nullptr, nullptr, true};
    case LangId::PHP:          return {"//", "/*", "*/", true};
    case LangId::Perl:         return {nullptr, nullptr, nullptr, true};
    case LangId::CSS:
    case LangId::SCSS:         return {"//", "/*", "*/", false};
    case LangId::HTML:
    case LangId::XML:          return {nullptr, "<!--", "-->", false};
    case LangId::JSON:         return {nullptr, nullptr, nullptr, false};
    case LangId::Diff:
    case LangId::Markdown:
    case LangId::Unknown:      return {"//", "/*", "*/", true};
    }
    return {"//", "/*", "*/", true};
}

// ── Language feature flags ───────────────────────────────────────────────────

struct LangFeatures {
    bool triple_quote_strings;  // Python """...""", '''...'''
    bool backtick_strings;      // JS `...` template literals, Go raw strings
    bool preprocessor;          // C/C++ #include, #define
    bool decorators;            // Python @, Java @, Rust #[...]
    bool shell_vars;            // $VAR, ${VAR}
    bool char_literals;         // 'c' is a char, not a string
    bool lifetime;              // Rust 'a lifetime annotations
    bool colon_atom;            // Ruby/Elixir :symbol
};

static LangFeatures features_for(LangId lang) {
    switch (lang) {
    case LangId::C:
    case LangId::Cpp:          return {false, false, true,  false, false, true,  false, false};
    case LangId::Python:       return {true,  false, false, true,  false, false, false, false};
    case LangId::Rust:         return {false, false, false, false, false, false, true,  false};
    case LangId::JavaScript:   return {false, true,  false, true,  false, false, false, false};
    case LangId::TypeScript:   return {false, true,  false, true,  false, false, false, false};
    case LangId::Go:           return {false, true,  false, false, false, true,  false, false};
    case LangId::Java:         return {false, false, false, true,  false, true,  false, false};
    case LangId::Kotlin:       return {false, false, false, true,  false, true,  false, false};
    case LangId::Swift:        return {false, false, false, true,  false, false, false, false};
    case LangId::Ruby:         return {false, false, false, false, false, false, false, true};
    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:   return {false, false, false, false, true,  false, false, false};
    case LangId::PHP:          return {false, false, false, false, true,  true,  false, false};
    case LangId::Perl:         return {false, false, false, false, true,  false, false, false};
    case LangId::Elixir:       return {false, false, false, true,  false, false, false, true};
    case LangId::Erlang:       return {false, false, false, false, false, true,  false, false};
    default:                   return {false, false, false, false, false, false, false, false};
    }
}

// ── Diff highlighter ─────────────────────────────────────────────────────────

static Element highlight_diff(const std::string& code) {
    std::string out;
    out.reserve(code.size());
    std::vector<StyledRun> runs;

    size_t i = 0;
    size_t n = code.size();

    while (i < n) {
        size_t line_start = i;
        // Find end of line (memchr is SIMD-accelerated in glibc)
        size_t eol = find_eol(code.data(), i, n);
        bool has_nl = (eol < n);
        size_t line_end = has_nl ? eol + 1 : eol;

        std::string_view line{code.data() + line_start, eol - line_start};
        size_t out_start = out.size();
        out.append(code, line_start, line_end - line_start);

        Style sty = syntax::plain();
        if (!line.empty()) {
            if (line[0] == '+')                                    sty = syntax::diff_add();
            else if (line[0] == '-')                               sty = syntax::diff_del();
            else if (line.starts_with("@@"))                       sty = syntax::diff_hunk();
            else if (line.starts_with("diff ") ||
                     line.starts_with("index ") ||
                     line.starts_with("--- ") ||
                     line.starts_with("+++ "))                     sty = syntax::diff_meta();
        }
        runs.push_back({out_start, line_end - line_start, sty});
        i = line_end;
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = syntax::plain(),
        .runs = std::move(runs),
    }};
}

// ── Main tokenizer ───────────────────────────────────────────────────────────

static inline bool is_punct_char(char c) {
    return kPunctChar[static_cast<unsigned char>(c)];
}

static inline bool is_op_char(char c) {
    return kOpChar[static_cast<unsigned char>(c)];
}

static Element highlight_code(const std::string& code, const std::string& lang_tag) {
    LangId lang = detect_lang(lang_tag);

    // Special case: diff gets its own highlighter
    if (lang == LangId::Diff) return highlight_diff(code);

    auto cs = comment_style_for(lang);
    auto feat = features_for(lang);

    std::string out;
    out.reserve(code.size());
    std::vector<StyledRun> runs;

    size_t i = 0;
    size_t n = code.size();

    auto emit = [&](size_t start, size_t byte_len, Style sty) {
        if (byte_len == 0) return;
        runs.push_back({start, byte_len, sty});
    };

    while (i < n) {
        char ch = code[i];

        // ── Newline ──────────────────────────────────────────────────
        if (ch == '\n') {
            size_t s = out.size();
            out += '\n';
            emit(s, 1, syntax::plain());
            ++i;
            continue;
        }

        // ── Whitespace ───────────────────────────────────────────────
        if (ch == ' ' || ch == '\t') {
            size_t s = out.size();
            size_t j = i;
            while (j < n && (code[j] == ' ' || code[j] == '\t')) ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::plain());
            i = j;
            continue;
        }

        // ── Preprocessor: #include, #define, etc. ────────────────────
        if (feat.preprocessor && ch == '#') {
            // Check if at start of line (or start of code)
            bool at_line_start = (i == 0 || code[i - 1] == '\n');
            if (at_line_start) {
                size_t s = out.size();
                size_t j = find_eol(code.data(), i, n);
                out.append(code, i, j - i);
                emit(s, j - i, syntax::preproc());
                i = j;
                continue;
            }
        }

        // ── Line comment: // or # or -- ──────────────────────────────
        if (cs.line && !std::string_view(cs.line).empty()) {
            std::string_view lc{cs.line};
            if (code.compare(i, lc.size(), lc) == 0) {
                size_t s = out.size();
                size_t j = find_eol(code.data(), i, n);
                out.append(code, i, j - i);
                emit(s, j - i, syntax::comment());
                i = j;
                continue;
            }
        }
        if (cs.hash_comment && ch == '#') {
            size_t s = out.size();
            size_t j = find_eol(code.data(), i, n);
            out.append(code, i, j - i);
            emit(s, j - i, syntax::comment());
            i = j;
            continue;
        }

        // ── Block comment: /* ... */, <!-- ... -->, {- ... -} ────────
        if (cs.block_open) {
            std::string_view bo{cs.block_open};
            std::string_view bc{cs.block_close};
            if (code.compare(i, bo.size(), bo) == 0) {
                size_t s = out.size();
                size_t j = i + bo.size();
                while (j + bc.size() <= n &&
                       code.compare(j, bc.size(), bc) != 0)
                    ++j;
                if (j + bc.size() <= n) j += bc.size();
                out.append(code, i, j - i);
                emit(s, j - i, syntax::comment());
                i = j;
                continue;
            }
        }

        // ── Decorators/attributes: @decorator, #[attr] ──────────────
        if (feat.decorators && ch == '@') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_' || code[j] == '.'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::attr());
            i = j;
            continue;
        }

        // ── Rust lifetime: 'a, 'static ──────────────────────────────
        if (feat.lifetime && ch == '\'' &&
            i + 1 < n && std::isalpha(static_cast<unsigned char>(code[i + 1]))) {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::type());
            i = j;
            continue;
        }

        // ── Shell variables: $VAR, ${VAR}, $(...) ────────────────────
        if (feat.shell_vars && ch == '$' && i + 1 < n) {
            size_t s = out.size();
            size_t j = i + 1;
            if (code[j] == '{') {
                // ${VAR}
                ++j;
                while (j < n && code[j] != '}') ++j;
                if (j < n) ++j;
            } else if (code[j] == '(') {
                // $(...) — just highlight the $( and )
                out.append(code, i, 2);
                emit(s, 2, syntax::shellvar());
                i += 2;
                continue;
            } else if (std::isalpha(static_cast<unsigned char>(code[j])) ||
                       code[j] == '_') {
                while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                                  code[j] == '_'))
                    ++j;
            } else {
                // $? $# $@ etc.
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::shellvar());
            i = j;
            continue;
        }

        // ── Triple-quoted strings: """...""" / '''...''' ─────────────
        if (feat.triple_quote_strings &&
            (ch == '"' || ch == '\'') &&
            i + 2 < n && code[i + 1] == ch && code[i + 2] == ch) {
            char q = ch;
            size_t s = out.size();
            size_t j = i + 3;
            while (j + 2 < n) {
                if (code[j] == '\\') { j += 2; continue; }
                if (code[j] == q && code[j + 1] == q && code[j + 2] == q) {
                    j += 3;
                    break;
                }
                ++j;
            }
            if (j + 2 >= n && !(j >= 3 && code[j-1] == q && code[j-2] == q && code[j-3] == q))
                j = n;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Backtick template literals: `...${...}...` ──────────────
        if (feat.backtick_strings && ch == '`') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '`') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                ++j;
            }
            if (j < n) ++j; // consume closing `
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── String literals: "..." ───────────────────────────────────
        if (ch == '"') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == '"') { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Char literal or string: '...' ────────────────────────────
        if (ch == '\'') {
            if (feat.char_literals) {
                // C-style char: 'x' or '\n' — short
                size_t s = out.size();
                size_t j = i + 1;
                if (j < n && code[j] == '\\') j += 2;
                else if (j < n) ++j;
                if (j < n && code[j] == '\'') ++j;
                out.append(code, i, j - i);
                emit(s, j - i, syntax::str());
                i = j;
                continue;
            }
            // Treat as string in Ruby, Python (single-quoted), etc.
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == '\'') { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Ruby/Elixir atom: :symbol ────────────────────────────────
        if (feat.colon_atom && ch == ':' &&
            i + 1 < n && std::isalpha(static_cast<unsigned char>(code[i + 1]))) {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_' || code[j] == '?' || code[j] == '!'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::constant());
            i = j;
            continue;
        }

        // ── Number: 0x..., 0b..., 0o..., digits[.digits][e...] ──────
        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            (ch == '.' && i + 1 < n &&
             std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
            size_t s = out.size();
            size_t j = i;
            if (ch == '0' && j + 1 < n) {
                char next = code[j + 1];
                if (next == 'x' || next == 'X') {
                    j += 2;
                    while (j < n && (std::isxdigit(static_cast<unsigned char>(code[j])) ||
                                      code[j] == '_'))
                        ++j;
                } else if (next == 'b' || next == 'B') {
                    j += 2;
                    while (j < n && (code[j] == '0' || code[j] == '1' || code[j] == '_'))
                        ++j;
                } else if (next == 'o' || next == 'O') {
                    j += 2;
                    while (j < n && ((code[j] >= '0' && code[j] <= '7') || code[j] == '_'))
                        ++j;
                } else goto decimal;
            } else {
            decimal:
                while (j < n && (std::isdigit(static_cast<unsigned char>(code[j])) ||
                                  code[j] == '_'))
                    ++j;
                if (j < n && code[j] == '.' && j + 1 < n &&
                    std::isdigit(static_cast<unsigned char>(code[j + 1]))) {
                    ++j;
                    while (j < n && (std::isdigit(static_cast<unsigned char>(code[j])) ||
                                      code[j] == '_'))
                        ++j;
                }
                // Exponent
                if (j < n && (code[j] == 'e' || code[j] == 'E')) {
                    ++j;
                    if (j < n && (code[j] == '+' || code[j] == '-')) ++j;
                    while (j < n && std::isdigit(static_cast<unsigned char>(code[j]))) ++j;
                }
            }
            // Number suffix: f, u, l, i32, etc.
            while (j < n && (std::isalpha(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::num());
            i = j;
            continue;
        }

        // ── Identifier / keyword / type / function ───────────────────
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            // Rust macros: name!
            if (lang == LangId::Rust && j < n && code[j] == '!')
                ++j;

            std::string_view word{code.data() + i, j - i};
            bool is_fn_call = (j < n && code[j] == '(');

            size_t s = out.size();
            out.append(code, i, j - i);

            auto wc = classify_word(word, lang);
            if (wc.constant)      emit(s, j - i, syntax::constant());
            else if (wc.keyword)  emit(s, j - i, syntax::kw());
            else if (wc.type)     emit(s, j - i, syntax::type());
            else if (is_fn_call)  emit(s, j - i, syntax::fn());
            else if (!word.empty() &&
                     std::isupper(static_cast<unsigned char>(word[0])) &&
                     word.size() > 1)
                                  emit(s, j - i, syntax::type());
            else                  emit(s, j - i, syntax::plain());

            i = j;
            continue;
        }

        // ── Multi-char operators: =>, ->, ::, |>, <-, ==, != etc. ───
        if (is_op_char(ch)) {
            size_t s = out.size();
            size_t j = i;
            // Consume runs of operator chars (max 3 for things like >>=)
            while (j < n && is_op_char(code[j]) && (j - i) < 3) ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::op());
            i = j;
            continue;
        }

        // ── Punctuation ──────────────────────────────────────────────
        if (is_punct_char(ch)) {
            size_t s = out.size();
            out += ch;
            emit(s, 1, syntax::punct());
            ++i;
            continue;
        }

        // ── Anything else — plain ────────────────────────────────────
        {
            size_t s = out.size();
            // Consume UTF-8 continuation bytes together
            size_t j = i + 1;
            if (static_cast<unsigned char>(ch) >= 0x80) {
                while (j < n && (static_cast<unsigned char>(code[j]) & 0xC0) == 0x80) ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::plain());
            i = j;
        }
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = syntax::plain(),
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
            auto sty = Style{}.with_fg(colors::code_fg);
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
        // Check for code/math fence toggle — only at line start
        bool at_line_start = (i == 0 || source_[i - 1] == '\n');
        if (at_line_start) {
            bool is_code_fence = i + 3 <= source_.size() &&
                ((source_[i] == '`' && source_[i+1] == '`' && source_[i+2] == '`') ||
                 (source_[i] == '~' && source_[i+1] == '~' && source_[i+2] == '~'));
            bool is_math_fence = !is_code_fence && i + 2 <= source_.size() &&
                source_[i] == '$' && source_[i+1] == '$';
            if (is_code_fence || is_math_fence) {
                size_t eol = source_.find('\n', i);
                if (eol == std::string::npos) break;
                in_fence = !in_fence;
                i = eol + 1;
                if (!in_fence) {
                    last_boundary = i;
                }
                continue;
            }
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
                if (c == '#' || c == '>' || c == '-' || c == '*' || c == '+' ||
                    c == '|' || c == '$') {
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
            bool at_line_start = (j == 0 || source_[j - 1] == '\n');
            if (at_line_start) {
                bool is_code = j + 3 <= boundary &&
                    ((source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') ||
                     (source_[j] == '~' && source_[j+1] == '~' && source_[j+2] == '~'));
                bool is_math = !is_code && j + 2 <= boundary &&
                    source_[j] == '$' && source_[j+1] == '$';
                if (is_code || is_math)
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
            bool at_line_start = (j == 0 || source_[j - 1] == '\n');
            if (at_line_start) {
                bool is_code = j + 3 <= boundary &&
                    ((source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') ||
                     (source_[j] == '~' && source_[j+1] == '~' && source_[j+2] == '~'));
                bool is_math = !is_code && j + 2 <= boundary &&
                    source_[j] == '$' && source_[j+1] == '$';
                if (is_code || is_math)
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
