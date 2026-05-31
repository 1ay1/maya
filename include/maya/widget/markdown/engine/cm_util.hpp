#pragma once
// cm_util.hpp — engine-private utilities for the CommonMark core.
//
// Spec-precise character classification, UTF-8 decode, percent/entity
// normalization, and small string helpers shared by the block- and
// inline-structure phases. All free functions in the engine namespace;
// no state. Private to the markdown implementation; NOT installed.

#include <cstdint>
#include <string>
#include <string_view>

#include "maya/widget/markdown/spec_chars.hpp"
#include "maya/widget/markdown/engine/cm_entities_table.hpp"

namespace maya::md_detail::engine {

namespace chars = ::maya::md_detail::chars;

using chars::is_ascii_alpha;
using chars::is_ascii_alnum;
using chars::is_ascii_digit;
using chars::is_ascii_punct;

constexpr char NUL = '\0';

[[nodiscard]] inline bool is_space_or_tab(char c) noexcept {
    return c == ' ' || c == '\t';
}

// Unicode-whitespace per the spec's needs: ASCII ws covers \t\n\f\r' '.
// For flanking we also treat the Unicode-space code points; we approximate
// at byte level (sufficient for the spec example set which uses U+00A0).
[[nodiscard]] inline bool is_unicode_ws_byte(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

// ── line helpers ────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string_view rstrip(std::string_view s) {
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' ||
            s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline std::string_view lstrip(std::string_view s) {
    while (!s.empty() && is_space_or_tab(s.front())) s.remove_prefix(1);
    return s;
}

[[nodiscard]] inline std::string_view strip(std::string_view s) {
    return rstrip(lstrip(s));
}

[[nodiscard]] inline bool is_blank(std::string_view s) {
    for (char c : s)
        if (!is_space_or_tab(c) && c != '\n' && c != '\r') return false;
    return true;
}

// Count leading indentation in columns: space = 1, tab = advance to next
// multiple of 4. Returns columns and sets `bytes` to chars consumed.
[[nodiscard]] inline int leading_indent(std::string_view s, std::size_t& bytes) {
    int col = 0;
    std::size_t i = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == ' ') ++col;
        else if (s[i] == '\t') col += 4 - (col % 4);
        else break;
    }
    bytes = i;
    return col;
}

// ── case folding ────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out += c;
    }
    return out;
}

// CommonMark reference-label normalization (§6.3): trim, collapse internal
// whitespace runs to one space, Unicode case-fold (we ASCII-lower).
[[nodiscard]] inline std::string normalize_label(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    bool prev_ws = false;
    bool started = false;
    for (char c : label) {
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (ws) {
            prev_ws = true;
            continue;
        }
        if (prev_ws && started) out += ' ';
        prev_ws = false;
        started = true;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out += c;
    }
    return out;
}

// ── UTF-8 ───────────────────────────────────────────────────────────────────

// Append code point `cp` as UTF-8. Invalid / zero → U+FFFD per spec.
inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ── entity resolution (§6.2) ────────────────────────────────────────────────

// Look up a named entity (without leading & / trailing ;). Returns the UTF-8
// bytes, or empty if unknown.
[[nodiscard]] inline std::string_view lookup_entity(std::string_view name) {
    int lo = 0, hi = kEntityCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        auto cmp = kEntities[mid].name.compare(name);
        if (cmp == 0) return kEntities[mid].utf8;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return {};
}

// Attempt to decode an entity / numeric character reference beginning at
// text[i] (text[i] == '&'). On success appends the decoded UTF-8 to `out`
// and returns the number of input bytes consumed (incl. & and ;). On
// failure returns 0 (caller emits '&' literally).
[[nodiscard]] inline std::size_t decode_entity(std::string_view text,
                                               std::size_t i, std::string& out) {
    if (i >= text.size() || text[i] != '&') return 0;
    std::size_t j = i + 1;
    if (j < text.size() && text[j] == '#') {
        // numeric
        ++j;
        uint32_t cp = 0;
        bool hex = false;
        std::size_t digits = 0;
        if (j < text.size() && (text[j] == 'x' || text[j] == 'X')) {
            hex = true;
            ++j;
            while (j < text.size() && digits < 7 &&
                   std::isxdigit(static_cast<unsigned char>(text[j]))) {
                char c = text[j];
                int v = (c >= '0' && c <= '9') ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                 : c - 'A' + 10;
                cp = cp * 16 + static_cast<uint32_t>(v);
                ++j;
                ++digits;
            }
        } else {
            while (j < text.size() && digits < 8 && is_ascii_digit(text[j])) {
                cp = cp * 10 + static_cast<uint32_t>(text[j] - '0');
                ++j;
                ++digits;
            }
        }
        (void)hex;
        if (digits == 0 || j >= text.size() || text[j] != ';') return 0;
        ++j;  // consume ;
        append_utf8(out, cp);
        return j - i;
    }
    // named: &name;
    std::size_t k = j;
    while (k < text.size() && is_ascii_alnum(text[k])) ++k;
    if (k == j || k >= text.size() || text[k] != ';') return 0;
    auto name = text.substr(j, k - j);
    auto utf8 = lookup_entity(name);
    if (utf8.empty()) return 0;
    out += utf8;
    return (k + 1) - i;
}

} // namespace maya::md_detail::engine
