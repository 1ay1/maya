// html_tag.cpp — HTML tag scanning shared by the inline and block parsers.
//
// Carved out of parser.cpp. Both the inline parser (allow-listed inline tags
// like <kbd>/<strong>/<a>) and the block parser (<details>, the §4.6 HTML
// block path) need to recognize a single HTML tag and find its matching
// closer, so this lives in its own TU with the shape declared in
// internal.hpp (HtmlTagInfo).
//
// Recognizes one tag start `<name …>` / `</name>` / `<name/>`, lowercasing
// the name and capturing the title/id/href attributes the renderer cares
// about. Unknown/malformed input returns an unmatched HtmlTagInfo so callers
// fall back to literal text.

#include <cctype>
#include <string>
#include <string_view>

#include "maya/widget/markdown/internal.hpp"

namespace maya {
namespace md_detail {

namespace {
std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // anonymous namespace

HtmlTagInfo try_parse_html_tag(std::string_view text, size_t start) {
    HtmlTagInfo info;
    if (start >= text.size() || text[start] != '<') return info;
    size_t i = start + 1;
    bool closer = false;
    if (i < text.size() && text[i] == '/') { closer = true; ++i; }
    if (i >= text.size()) return info;
    char first = text[i];
    if (!(std::isalpha(static_cast<unsigned char>(first)))) return info;

    size_t name_start = i;
    while (i < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '-')) {
        ++i;
    }
    auto tag_name = ascii_lower(text.substr(name_start, i - name_start));

    // Parse attributes: `name="value"` pairs, cheap whitespace-delimited.
    while (i < text.size() && text[i] != '>' && text[i] != '/') {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i < text.size() && (text[i] == '>' || text[i] == '/')) break;
        if (i >= text.size()) return info;
        size_t an_start = i;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) ||
                                   text[i] == '-' || text[i] == '_')) ++i;
        if (i == an_start) return info;                      // malformed
        auto attr_name = ascii_lower(text.substr(an_start, i - an_start));
        std::string attr_val;
        if (i < text.size() && text[i] == '=') {
            ++i;
            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                char q = text[i++];
                size_t limit = std::min(text.size(), i + 500);
                while (i < limit && text[i] != q) {
                    if (text[i] == '\\' && i + 1 < text.size()) {
                        attr_val += text[i + 1]; i += 2; continue;
                    }
                    attr_val += text[i++];
                }
                if (i < text.size() && text[i] == q) ++i;
            } else {
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
                       text[i] != '>' && text[i] != '/') {
                    attr_val += text[i++];
                }
            }
        }
        if (attr_name == "title") info.attr_title = std::move(attr_val);
        else if (attr_name == "id") info.attr_id = std::move(attr_val);
        else if (attr_name == "href") info.attr_href = std::move(attr_val);
    }
    bool self_closing = false;
    if (i < text.size() && text[i] == '/') { self_closing = true; ++i; }
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size() || text[i] != '>') return info;

    info.matched      = true;
    info.is_closer    = closer;
    info.self_closing = self_closing;
    info.name         = std::move(tag_name);
    info.end          = i + 1;
    return info;
}

// Find the matching closer `</tag>` for a paired HTML tag. Bounded scan.
size_t find_html_closer(std::string_view text, size_t start,
                        std::string_view tag, size_t max_dist) {
    size_t limit = std::min(text.size(), start + max_dist);
    size_t i = start;
    while (i < limit) {
        if (text[i] == '<' && i + 1 < text.size() && text[i + 1] == '/') {
            auto info = try_parse_html_tag(text, i);
            if (info.matched && info.is_closer && info.name == tag) return i;
        }
        ++i;
    }
    return std::string_view::npos;
}

} // namespace md_detail
} // namespace maya
