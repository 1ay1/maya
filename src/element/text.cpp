#include "maya/element/text.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

// ============================================================================
// Unicode width utilities
// ============================================================================

char32_t decode_utf8(std::string_view sv, std::size_t& pos) noexcept {
    if (pos >= sv.size()) return 0;

    auto byte = static_cast<uint8_t>(sv[pos]);

    // ASCII fast path
    if (byte < 0x80) {
        ++pos;
        return static_cast<char32_t>(byte);
    }

    int len = 0;
    char32_t cp = 0;
    if ((byte & 0xE0) == 0xC0)      { len = 2; cp = byte & 0x1F; }
    else if ((byte & 0xF0) == 0xE0) { len = 3; cp = byte & 0x0F; }
    else if ((byte & 0xF8) == 0xF0) { len = 4; cp = byte & 0x07; }
    else { ++pos; return 0xFFFD; }

    if (pos + len > sv.size()) { ++pos; return 0xFFFD; }

    for (int i = 1; i < len; ++i) {
        auto cont = static_cast<uint8_t>(sv[pos + i]);
        if ((cont & 0xC0) != 0x80) { pos += i; return 0xFFFD; }
        cp = (cp << 6) | (cont & 0x3F);
    }
    pos += len;
    return cp;
}

/// Compute the display width of a single decoded codepoint.
/// Returns 0 for control/zero-width characters, 2 for wide, 1 otherwise.
/// Returns -1 for codepoints that should be skipped entirely.
constexpr int codepoint_width(char32_t cp) noexcept {
    if (cp < 0x20) return -1;                         // control chars
    if (cp == 0x200D) return -1;                       // zero-width joiner
    if (cp == 0xFE0F || cp == 0xFE0E) return -1;      // variation selectors
    if (cp >= 0x1160 && cp <= 0x11FF) return -1;       // Hangul jungseong/jongseong
    if (cp >= 0x200B && cp <= 0x200F) return -1;       // zero-width spaces / directional marks
    if (cp >= 0x0300 && cp <= 0x036F) return -1;       // combining diacritical marks
    if (cp == 0xFEFF) return -1;                       // BOM / zero-width no-break space
    return is_wide_char(cp) ? 2 : 1;
}

int string_width(std::string_view text) noexcept {
    int width = 0;
    std::size_t pos = 0;
    const std::size_t len = text.size();
    const char* data = text.data();

    // ASCII fast path: scan runs of ASCII bytes (common for English/code).
    // Each ASCII byte >= 0x20 is exactly 1 column wide.
    while (pos < len) {
        // Batch ASCII characters — no decode needed.
        std::size_t ascii_start = pos;
        while (pos < len && static_cast<unsigned char>(data[pos]) < 0x80
                         && static_cast<unsigned char>(data[pos]) >= 0x20) {
            ++pos;
        }
        width += static_cast<int>(pos - ascii_start);

        // Handle non-ASCII or control characters one at a time.
        if (pos < len && (static_cast<unsigned char>(data[pos]) >= 0x80
                       || static_cast<unsigned char>(data[pos]) < 0x20)) {
            char32_t cp = decode_utf8(text, pos);
            int w = codepoint_width(cp);
            if (w > 0) width += w;
        }
    }
    return width;
}

// ============================================================================
// word_wrap
// ============================================================================

namespace {

/// Encapsulates the mutable state for wrapping a single logical line.
struct LineWrapper {
    std::string_view line;
    int              max_width;

    std::size_t pos                = 0;
    std::size_t current_line_start = 0;
    int         current_width      = 0;
    std::size_t last_break         = std::string_view::npos;

    void emit_lines(std::vector<std::string_view>& out) {
        while (pos < line.size()) {
            std::size_t char_start = pos;
            char32_t cp = decode_utf8(line, pos);
            int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);

            // Whitespace and hyphens are potential break points.
            if (cp == ' ' || cp == '\t' || cp == '-') {
                last_break = pos; // break *after* the character
            }

            if (current_width + cw > max_width && current_width > 0) {
                if (last_break != std::string_view::npos && last_break > current_line_start) {
                    // Break at last whitespace/hyphen.
                    out.emplace_back(line.substr(current_line_start, last_break - current_line_start));
                    // Skip leading spaces on next line.
                    std::size_t next = last_break;
                    while (next < line.size() && line[next] == ' ') ++next;
                    current_line_start = next;
                    pos = next;
                } else {
                    // Force break at current character.
                    out.emplace_back(line.substr(current_line_start, char_start - current_line_start));
                    current_line_start = char_start;
                    pos = char_start;
                }
                current_width = 0;
                last_break = std::string_view::npos;
                continue;
            }

            current_width += cw;
        }

        // Remaining content.
        if (current_line_start < line.size()) {
            out.emplace_back(line.substr(current_line_start));
        }
    }
};

} // anonymous namespace

std::vector<std::string_view>
word_wrap(std::string_view text, int max_width) {
    if (max_width <= 0) max_width = 1;

    std::vector<std::string_view> lines;

    // Split on explicit newlines first.
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        auto nl = text.find('\n', line_start);
        auto line = (nl == std::string_view::npos)
                  ? text.substr(line_start)
                  : text.substr(line_start, nl - line_start);

        if (line.empty()) {
            lines.emplace_back(line);
        } else {
            LineWrapper wrapper{.line = line, .max_width = max_width};
            wrapper.emit_lines(lines);
        }

        if (nl == std::string_view::npos) break;
        line_start = nl + 1;
    }

    if (lines.empty()) {
        lines.emplace_back(std::string_view{});
    }

    return lines;
}

// ============================================================================
// Truncation helpers
// ============================================================================

namespace {

/// Decoded codepoint with its byte position and display width.
struct CodepointInfo {
    std::size_t offset;
    std::size_t len;
    int         width;
};

/// Decode all codepoints from text into a vector of CodepointInfo.
[[nodiscard]] std::vector<CodepointInfo> decode_all(std::string_view text) {
    std::vector<CodepointInfo> result;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = pos;
        char32_t cp = decode_utf8(text, pos);
        int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);
        result.push_back({start, pos - start, cw});
    }
    return result;
}

/// Collect codepoints from the end of a decoded sequence that fit within a budget.
/// Returns the index of the first kept codepoint.
[[nodiscard]] std::size_t find_tail_start(
    const std::vector<CodepointInfo>& chars, int budget) {
    int w = 0;
    std::size_t first_kept = chars.size();
    for (const auto& ch : chars | std::views::reverse) {
        if (w + ch.width > budget) break;
        w += ch.width;
        --first_kept;
    }
    return first_kept;
}

inline constexpr std::string_view ellipsis = "\xe2\x80\xa6"; // U+2026

} // anonymous namespace

namespace detail {

std::string
truncate_end(std::string_view text, int max_width) {
    if (max_width <= 0) return {};
    if (string_width(text) <= max_width) return std::string{text};
    if (max_width == 1) return std::string{ellipsis};

    int budget = max_width - 1; // reserve 1 column for ellipsis
    std::string result;
    int w = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = pos;
        char32_t cp = decode_utf8(text, pos);
        int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);
        if (w + cw > budget) break;
        result.append(text.data() + start, pos - start);
        w += cw;
    }
    result += ellipsis;
    return result;
}

std::string
truncate_start(std::string_view text, int max_width) {
    if (max_width <= 0) return {};
    if (string_width(text) <= max_width) return std::string{text};
    if (max_width == 1) return std::string{ellipsis};

    int budget = max_width - 1;
    auto chars = decode_all(text);
    std::size_t first_kept = find_tail_start(chars, budget);

    std::string result;
    result.reserve(3 + (text.size() - chars[first_kept].offset));
    result += ellipsis;
    if (first_kept < chars.size()) {
        result.append(text.data() + chars[first_kept].offset,
                      text.size() - chars[first_kept].offset);
    }
    return result;
}

std::string
truncate_middle(std::string_view text, int max_width) {
    if (max_width <= 0) return {};
    if (string_width(text) <= max_width) return std::string{text};
    if (max_width <= 2) return truncate_end(text, max_width);

    int left_budget  = (max_width - 1) / 2;
    int right_budget = max_width - 1 - left_budget;

    // Collect left portion.
    std::string left_part;
    int lw = 0;
    std::size_t pos = 0;
    while (pos < text.size() && lw < left_budget) {
        std::size_t start = pos;
        char32_t cp = decode_utf8(text, pos);
        int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);
        if (lw + cw > left_budget) break;
        left_part.append(text.data() + start, pos - start);
        lw += cw;
    }

    // Collect right portion using shared helper.
    auto chars = decode_all(text);
    std::size_t first_right = find_tail_start(chars, right_budget);

    std::string result;
    result.reserve(left_part.size() + 3 + (text.size() - chars[first_right].offset));
    result  = std::move(left_part);
    result += ellipsis;
    if (first_right < chars.size()) {
        result.append(text.data() + chars[first_right].offset,
                      text.size() - chars[first_right].offset);
    }
    return result;
}

} // namespace detail

// ============================================================================
// TextElement methods
// ============================================================================

Size TextElement::measure(int max_width) const {
    if (content.empty()) {
        return {Columns{0}, Rows{1}};
    }

    // Cache hit: same width + wrap + content length → reuse prior result.
    // Content identity is trusted via size — callers don't mutate content
    // of a TextElement after handing it to the renderer.
    if (cached_width == max_width
        && cached_wrap == wrap
        && cached_content_size == content.size()
        && cached_size.height.value > 0) {
        return cached_size;
    }

    // Miss: populate the cache via format(), which stores both lines and size.
    (void)format(max_width);
    return cached_size;
}

const std::vector<std::string>& TextElement::format(int max_width) const {
    // Cache hit: same width + wrap + content length → return cached lines.
    if (cached_width == max_width
        && cached_wrap == wrap
        && cached_content_size == content.size()
        && (!cached_lines.empty() || content.empty())) {
        return cached_lines;
    }

    cached_lines.clear();

    if (content.empty()) {
        cached_lines.emplace_back("");
        cached_size = {Columns{0}, Rows{1}};
    } else {
        switch (wrap) {
            case TextWrap::Wrap: {
                auto views = word_wrap(content, max_width);
                cached_lines.reserve(views.size());
                int widest = 0;
                for (auto& v : views) {
                    // ASCII fast path: byte length == display width.
                    std::size_t len = v.size();
                    bool all_ascii = true;
                    for (std::size_t i = 0; i < len; ++i) {
                        if (static_cast<unsigned char>(v[i]) >= 0x80) {
                            all_ascii = false;
                            break;
                        }
                    }
                    int w = all_ascii ? static_cast<int>(len) : string_width(v);
                    if (w > widest) widest = w;
                    cached_lines.emplace_back(v);
                }
                cached_size = {Columns{widest},
                               Rows{static_cast<int>(cached_lines.size())}};
                break;
            }

            case TextWrap::NoWrap: {
                int widest = 0;
                for (auto part : content | std::views::split('\n')) {
                    std::string_view sv{part};
                    int w = string_width(sv);
                    if (w > widest) widest = w;
                    cached_lines.emplace_back(sv);
                }
                if (cached_lines.empty()) cached_lines.emplace_back("");
                cached_size = {Columns{widest},
                               Rows{static_cast<int>(cached_lines.size())}};
                break;
            }

            case TextWrap::TruncateEnd:
                cached_lines.emplace_back(detail::truncate_end(content, max_width));
                cached_size = {Columns{std::min(string_width(content), max_width)},
                               Rows{1}};
                break;

            case TextWrap::TruncateStart:
                cached_lines.emplace_back(detail::truncate_start(content, max_width));
                cached_size = {Columns{std::min(string_width(content), max_width)},
                               Rows{1}};
                break;

            case TextWrap::TruncateMiddle:
                cached_lines.emplace_back(detail::truncate_middle(content, max_width));
                cached_size = {Columns{std::min(string_width(content), max_width)},
                               Rows{1}};
                break;
        }
    }

    cached_width = max_width;
    cached_wrap = wrap;
    cached_content_size = content.size();
    return cached_lines;
}

} // namespace maya
