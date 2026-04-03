#pragma once
// maya::element::text - Text element: a leaf node that displays styled text
//
// Handles word wrapping, truncation, and Unicode-aware width computation.
// TextElement satisfies the Measurable concept: given a max_width constraint,
// it returns the Size it would occupy.

#include "../core/types.hpp"
#include "../style/style.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

// ============================================================================
// TextWrap - How text reflows when it exceeds available width
// ============================================================================

enum class TextWrap : uint8_t {
    Wrap,             ///< Break at word boundaries (default).
    TruncateEnd,      ///< Cut from the right, append ellipsis.
    TruncateMiddle,   ///< Keep start and end, ellipsis in center.
    TruncateStart,    ///< Cut from the left, prepend ellipsis.
    NoWrap,           ///< Never break; may overflow the container.
};

// ============================================================================
// Unicode width utilities
// ============================================================================
// A simplified CJK / fullwidth character detector. Covers the most common
// ranges used in terminal rendering. East Asian Wide (W) and Fullwidth (F)
// code points occupy two terminal columns; everything else occupies one.

/// Returns true if the Unicode code point is a wide character (CJK, etc.)
/// that occupies two terminal columns.
/// Ranges match the East Asian Width property (W=Wide, F=Fullwidth) from
/// Unicode, aligned with the reference implementation in Claude Code ($N1).
[[nodiscard]] constexpr bool is_wide_char(char32_t cp) noexcept {
    // Hangul Jamo (leading consonants) — wide in most terminals
    if (cp >= 0x1100  && cp <= 0x115F)  return true;
    // CJK Radicals Supplement, Kangxi Radicals, Ideographic Description,
    // CJK Symbols & Punct, Hiragana, Katakana, Bopomofo, Hangul Compat Jamo,
    // Kanbun, Bopomofo Extended, CJK Strokes (U+2E80-U+3247)
    if (cp >= 0x2E80  && cp <= 0x3247)  return true;
    // CJK angle brackets
    if (cp == 0x2329 || cp == 0x232A)   return true;
    // CJK Compat, Extension A, Yijing (U+3250-U+4DBF)
    if (cp >= 0x3250  && cp <= 0x4DBF)  return true;
    // CJK Unified Ideographs, Yi Syllables/Radicals (U+4E00-U+A4C6)
    if (cp >= 0x4E00  && cp <= 0xA4C6)  return true;
    // Hangul Syllables
    if (cp >= 0xAC00  && cp <= 0xD7AF)  return true;
    // CJK Compatibility Ideographs
    if (cp >= 0xF900  && cp <= 0xFAFF)  return true;
    // Fullwidth Latin, Greek, Cyrillic, etc.
    if (cp >= 0xFF01  && cp <= 0xFF60)  return true;
    // Fullwidth currency and other signs
    if (cp >= 0xFFE0  && cp <= 0xFFE6)  return true;
    // Enclosed Ideographic Supplement
    if (cp >= 0x1F200 && cp <= 0x1F251) return true;
    // CJK Unified Ideographs Extension B through end of TIP (U+20000-U+3FFFD)
    if (cp >= 0x20000 && cp <= 0x3FFFD) return true;
    return false;
}

/// Decode the next UTF-8 code point from a string_view.
/// Advances `pos` past the decoded bytes. Returns U+FFFD on invalid input.
[[nodiscard]] inline char32_t decode_utf8(std::string_view sv, std::size_t& pos) noexcept {
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

/// Compute the display width of a UTF-8 string in terminal columns.
/// Wide (CJK) characters count as 2; all others count as 1.
/// Control characters (< 0x20) and zero-width code points are ignored.
[[nodiscard]] inline int string_width(std::string_view text) noexcept {
    int width = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t cp = decode_utf8(text, pos);
        if (cp < 0x20) continue;                        // skip control chars
        if (cp == 0x200D) continue;                     // zero-width joiner (ZWJ)
        if (cp == 0xFE0F || cp == 0xFE0E) continue;    // variation selectors
        if (cp >= 0x1160 && cp <= 0x11FF) continue;     // Hangul jungseong/jongseong (zero-width)
        if (cp >= 0x200B && cp <= 0x200F) continue;     // zero-width spaces / directional marks
        if (cp >= 0x0300 && cp <= 0x036F) continue;     // combining diacritical marks (zero-width)
        if (cp == 0xFEFF) continue;                     // BOM / zero-width no-break space
        width += is_wide_char(cp) ? 2 : 1;
    }
    return width;
}

// ============================================================================
// word_wrap - Split text into lines that fit within a column budget
// ============================================================================
// Breaks at whitespace boundaries. If a single word exceeds max_width, it
// is force-broken at the column limit. Returns a vector of string_views
// pointing into the original text.

[[nodiscard]] inline std::vector<std::string_view>
word_wrap(std::string_view text, int max_width) {
    if (max_width <= 0) max_width = 1;

    std::vector<std::string_view> lines;

    // Split on explicit newlines first
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        auto nl = text.find('\n', line_start);
        auto line = (nl == std::string_view::npos)
                  ? text.substr(line_start)
                  : text.substr(line_start, nl - line_start);

        // Wrap this logical line
        if (line.empty()) {
            lines.emplace_back(line);
        } else {
            std::size_t pos = 0;
            std::size_t current_line_start = 0;
            int current_width = 0;
            std::size_t last_break = std::string_view::npos; // byte offset after the last space

            while (pos < line.size()) {
                std::size_t char_start = pos;
                char32_t cp = decode_utf8(line, pos);
                int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);

                // Whitespace and hyphens are potential break points.
                // For spaces/tabs: break after (trim leading spaces on next line).
                // For hyphens: break after (keep hyphen at end of line).
                if (cp == ' ' || cp == '\t' || cp == '-') {
                    last_break = pos; // break *after* the character
                }

                if (current_width + cw > max_width && current_width > 0) {
                    // Need to break
                    if (last_break != std::string_view::npos && last_break > current_line_start) {
                        // Break at last whitespace
                        lines.emplace_back(line.substr(current_line_start, last_break - current_line_start));
                        // Skip leading spaces on next line
                        std::size_t next = last_break;
                        while (next < line.size() && line[next] == ' ') ++next;
                        current_line_start = next;
                        pos = next;
                    } else {
                        // Force break at current character
                        lines.emplace_back(line.substr(current_line_start, char_start - current_line_start));
                        current_line_start = char_start;
                        pos = char_start;
                    }
                    current_width = 0;
                    last_break = std::string_view::npos;
                    continue;
                }

                current_width += cw;
            }

            // Remaining content
            if (current_line_start < line.size()) {
                lines.emplace_back(line.substr(current_line_start));
            } else if (current_line_start == line.size() && !lines.empty()) {
                // Edge case: trailing whitespace consumed exactly at boundary -- no empty line needed
            }
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

namespace detail {

/// Truncate a string to fit within `max_width` columns, appending an ellipsis.
[[nodiscard]] inline std::string
truncate_end(std::string_view text, int max_width) {
    if (max_width <= 0) return {};
    if (string_width(text) <= max_width) return std::string{text};
    if (max_width == 1) return "\xe2\x80\xa6"; // single ellipsis character

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
    result += "\xe2\x80\xa6"; // U+2026 HORIZONTAL ELLIPSIS
    return result;
}

/// Truncate from the start, prepending an ellipsis.
[[nodiscard]] inline std::string
truncate_start(std::string_view text, int max_width) {
    if (max_width <= 0) return {};
    if (string_width(text) <= max_width) return std::string{text};
    if (max_width == 1) return "\xe2\x80\xa6";

    int budget = max_width - 1;
    // Collect all char boundaries in one forward pass.
    struct CharInfo { std::size_t offset; std::size_t len; int width; };
    std::vector<CharInfo> chars;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = pos;
        char32_t cp = decode_utf8(text, pos);
        int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);
        chars.push_back({start, pos - start, cw});
    }

    // Walk backward, accumulating width until budget is hit.
    int w = 0;
    std::size_t first_kept = chars.size();
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
        if (w + it->width > budget) break;
        w += it->width;
        --first_kept;
    }

    // Build result in one pass: ellipsis + kept chars (O(N) total).
    std::string result;
    result.reserve(3 + (text.size() - chars[first_kept].offset));
    result += "\xe2\x80\xa6"; // U+2026 HORIZONTAL ELLIPSIS (3 UTF-8 bytes)
    if (first_kept < chars.size()) {
        result.append(text.data() + chars[first_kept].offset,
                      text.size() - chars[first_kept].offset);
    }
    return result;
}

/// Truncate from the middle, placing an ellipsis in the center.
[[nodiscard]] inline std::string
truncate_middle(std::string_view text, int max_width) {
    if (max_width <= 0) return {};
    if (string_width(text) <= max_width) return std::string{text};
    if (max_width <= 2) return truncate_end(text, max_width);

    int left_budget  = (max_width - 1) / 2;
    int right_budget = max_width - 1 - left_budget;

    // Collect left portion
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

    // Collect right portion (walk all chars, take from end)
    struct CharInfo { std::size_t offset; std::size_t len; int width; };
    std::vector<CharInfo> chars;
    std::size_t rpos = 0;
    while (rpos < text.size()) {
        std::size_t start = rpos;
        char32_t cp = decode_utf8(text, rpos);
        int cw = (cp < 0x20) ? 0 : (is_wide_char(cp) ? 2 : 1);
        chars.push_back({start, rpos - start, cw});
    }

    // Walk backward to find the start of the right portion (O(N), no insert).
    int rw = 0;
    std::size_t first_right = chars.size();
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
        if (rw + it->width > right_budget) break;
        rw += it->width;
        --first_right;
    }

    std::string result;
    result.reserve(left_part.size() + 3 + (text.size() - chars[first_right].offset));
    result  = left_part;
    result += "\xe2\x80\xa6";
    if (first_right < chars.size()) {
        result.append(text.data() + chars[first_right].offset,
                      text.size() - chars[first_right].offset);
    }
    return result;
}

} // namespace detail

// ============================================================================
// TextElement - A leaf element that displays styled text
// ============================================================================
// Satisfies the Measurable concept. During layout, measure() computes
// the Size this text would occupy given a column constraint.

struct TextElement {
    std::string content;
    Style       style;
    TextWrap    wrap = TextWrap::Wrap;

    // -- Measurement ---------------------------------------------------------

    /// Compute the Size this text would occupy given a maximum width constraint.
    /// Handles word wrapping, truncation, and Unicode-aware width.
    [[nodiscard]] Size measure(int max_width) const {
        if (content.empty()) {
            return {Columns{0}, Rows{1}};
        }

        switch (wrap) {
            case TextWrap::Wrap: {
                auto lines = word_wrap(content, max_width);
                int widest = 0;
                for (auto& line : lines) {
                    widest = std::max(widest, string_width(line));
                }
                return {Columns{widest}, Rows{static_cast<int>(lines.size())}};
            }

            case TextWrap::NoWrap: {
                // Count explicit newlines only
                int height = 1;
                int widest = 0;
                int current = 0;
                std::size_t pos = 0;
                while (pos < content.size()) {
                    if (content[pos] == '\n') {
                        widest = std::max(widest, current);
                        current = 0;
                        ++height;
                        ++pos;
                        continue;
                    }
                    std::size_t start = pos;
                    char32_t cp = decode_utf8(content, pos);
                    if (cp >= 0x20) current += is_wide_char(cp) ? 2 : 1;
                }
                widest = std::max(widest, current);
                return {Columns{widest}, Rows{height}};
            }

            case TextWrap::TruncateEnd:
            case TextWrap::TruncateStart:
            case TextWrap::TruncateMiddle: {
                // Truncation modes always produce a single line
                int raw_width = string_width(content);
                int clamped = std::min(raw_width, max_width);
                return {Columns{clamped}, Rows{1}};
            }
        }
        __builtin_unreachable();
    }

    // -- Rendering helpers ---------------------------------------------------

    /// Return the display-ready lines for a given width constraint.
    /// Word-wrapped or truncated as configured.
    [[nodiscard]] std::vector<std::string> format(int max_width) const {
        if (content.empty()) return {""};

        switch (wrap) {
            case TextWrap::Wrap: {
                auto views = word_wrap(content, max_width);
                std::vector<std::string> result;
                result.reserve(views.size());
                for (auto& v : views) result.emplace_back(v);
                return result;
            }

            case TextWrap::NoWrap: {
                std::vector<std::string> result;
                std::size_t start = 0;
                while (start <= content.size()) {
                    auto nl = content.find('\n', start);
                    if (nl == std::string::npos) {
                        result.emplace_back(content.substr(start));
                        break;
                    }
                    result.emplace_back(content.substr(start, nl - start));
                    start = nl + 1;
                }
                return result;
            }

            case TextWrap::TruncateEnd:
                return {detail::truncate_end(content, max_width)};

            case TextWrap::TruncateStart:
                return {detail::truncate_start(content, max_width)};

            case TextWrap::TruncateMiddle:
                return {detail::truncate_middle(content, max_width)};
        }
        __builtin_unreachable();
    }
};

} // namespace maya
