// parser.cpp — Markdown lexer + parser. Carved from markdown.cpp.
//
// Owns: compile-time char tables, HTML entity table, emoji shortcode
// table, URL/mention sniffer, inline parser (parse_inlines), table &
// list helpers, ref-def collector, block parser (parse_markdown_impl),
// and the public parse_markdown() entry point.
//
// Cross-TU symbols (parse_inlines, find_eol, collect_ref_defs,
// parse_markdown_impl) are re-exported under namespace maya::md_detail
// at the bottom of the file.

#include "maya/widget/markdown.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"

#include "maya/widget/markdown/internal.hpp"

namespace maya {

// ── Lifted out of the anonymous namespace so streaming.cpp's TU can
// share the same TLS slot via maya::md_detail::RefDefsScope. Bodies
// are byte-identical to the original — just no longer static / anon.
namespace md_detail {
// ── Reference-link map (threaded through parse via thread_local) ──────────
// parse_inlines is called from many nested places (emphasis recursion, table
// cells, list items, blockquote bodies).  Threading a pointer through every
// call site would touch the whole inline parser API.  Instead, the top-level
// parse_markdown stashes the active Document's ref_defs in this pointer.
thread_local const std::unordered_map<std::string, md::LinkRef>*
    tls_ref_defs = nullptr;

RefDefsScope::RefDefsScope(const std::unordered_map<std::string, md::LinkRef>* p) noexcept
    : prev(tls_ref_defs) { tls_ref_defs = p; }
RefDefsScope::~RefDefsScope() { tls_ref_defs = prev; }
} // namespace md_detail

// Re-publish the lifted names so the anon-namespace bodies below
// (lookup_ref, parse_inlines, parse_markdown) keep compiling without
// any source-level edit. tls_ref_defs is read by lookup_ref;
// RefDefsGuard is used locally inside parse_markdown().
using ::maya::md_detail::tls_ref_defs;
namespace { using RefDefsGuard = ::maya::md_detail::RefDefsScope; }

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
// URL-sniffing, mentions, entities and emoji shortcodes are handled in a
// separate post-pass over Text nodes (linkify_text_nodes), so their trigger
// chars (h/w/@/#/&/:) do NOT appear here — adding them would cost a stop
// per word in normal prose.
static constexpr auto kInlineSpecial = make_table<
    '`', '*', '_', '~', '[', '!', '\\', '<', '$',
    '=', '^'>();

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

inline std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}


inline const md::LinkRef* lookup_ref(std::string_view label) {
    if (!tls_ref_defs) return nullptr;
    auto key = ascii_lower(label);
    // Collapse whitespace runs to a single space (CommonMark normalization).
    std::string norm;
    norm.reserve(key.size());
    bool prev_space = false;
    for (char c : key) {
        bool sp = std::isspace(static_cast<unsigned char>(c));
        if (sp) {
            if (!prev_space && !norm.empty()) norm += ' ';
            prev_space = true;
        } else {
            norm += c;
            prev_space = false;
        }
    }
    while (!norm.empty() && norm.back() == ' ') norm.pop_back();
    auto it = tls_ref_defs->find(norm);
    return (it == tls_ref_defs->end()) ? nullptr : &it->second;
}

// ── HTML entity decoding ──────────────────────────────────────────────────
// Recognises the handful of named entities that appear in LLM output plus
// numeric (&#123; / &#xNN;) forms.  Returns true if `text[start..]` matched
// and consumes up to `end_out`; the decoded UTF-8 goes into `out`.

static bool append_utf8(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp < 0x80) { out += static_cast<char>(cp); return true; }
    if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
        return true;
    }
    if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
        return true;
    }
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
    return true;
}

struct NamedEntity { std::string_view name; std::string_view utf8; };
static constexpr std::array<NamedEntity, 40> kNamedEntities{{
    {"amp",    "&"},  {"lt",     "<"},  {"gt",    ">"},
    {"quot",   "\""}, {"apos",   "'"},  {"nbsp",  "\xC2\xA0"},
    {"copy",   "\xC2\xA9"},           {"reg",   "\xC2\xAE"},
    {"trade",  "\xE2\x84\xA2"},       {"sect",  "\xC2\xA7"},
    {"para",   "\xC2\xB6"},           {"deg",   "\xC2\xB0"},
    {"plusmn", "\xC2\xB1"},           {"times", "\xC3\x97"},
    {"divide", "\xC3\xB7"},           {"micro", "\xC2\xB5"},
    {"hellip", "\xE2\x80\xA6"},       {"mdash", "\xE2\x80\x94"},
    {"ndash",  "\xE2\x80\x93"},       {"lsquo", "\xE2\x80\x98"},
    {"rsquo",  "\xE2\x80\x99"},       {"ldquo", "\xE2\x80\x9C"},
    {"rdquo",  "\xE2\x80\x9D"},       {"laquo", "\xC2\xAB"},
    {"raquo",  "\xC2\xBB"},           {"bull",  "\xE2\x80\xA2"},
    {"dagger", "\xE2\x80\xA0"},       {"middot","\xC2\xB7"},
    {"larr",   "\xE2\x86\x90"},       {"uarr",  "\xE2\x86\x91"},
    {"rarr",   "\xE2\x86\x92"},       {"darr",  "\xE2\x86\x93"},
    {"harr",   "\xE2\x86\x94"},       {"infin", "\xE2\x88\x9E"},
    {"ne",     "\xE2\x89\xA0"},       {"le",    "\xE2\x89\xA4"},
    {"ge",     "\xE2\x89\xA5"},       {"pi",    "\xCF\x80"},
    {"check",  "\xE2\x9C\x93"},       {"cross", "\xE2\x9C\x97"},
}};

// Try to decode an HTML entity starting at text[start]='&'.  If successful
// returns true, writes decoded bytes into `out`, sets *consumed to the total
// byte count including & and ;.
static bool try_decode_entity(std::string_view text, size_t start,
                              std::string& out, size_t* consumed) {
    if (start >= text.size() || text[start] != '&') return false;
    // Scan to ';' within a short bound.
    size_t limit = std::min(text.size(), start + 10);
    size_t semi = std::string_view::npos;
    for (size_t k = start + 1; k < limit; ++k) {
        if (text[k] == ';') { semi = k; break; }
        if (text[k] == '&' || text[k] == ' ') break;
    }
    if (semi == std::string_view::npos) return false;
    auto body = text.substr(start + 1, semi - start - 1);
    if (body.empty()) return false;

    // Numeric: &#N; or &#xH;
    if (body[0] == '#') {
        if (body.size() < 2) return false;
        uint32_t cp = 0;
        if (body[1] == 'x' || body[1] == 'X') {
            if (body.size() < 3 || body.size() > 8) return false;
            for (size_t k = 2; k < body.size(); ++k) {
                char c = body[k];
                cp <<= 4;
                if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
                else return false;
            }
        } else {
            if (body.size() > 8) return false;
            for (size_t k = 1; k < body.size(); ++k) {
                char c = body[k];
                if (c < '0' || c > '9') return false;
                cp = cp * 10 + static_cast<uint32_t>(c - '0');
            }
        }
        if (!append_utf8(out, cp)) return false;
        *consumed = semi - start + 1;
        return true;
    }

    // Named entity — linear scan; table is tiny.
    for (auto& e : kNamedEntities) {
        if (body == e.name) {
            out.append(e.utf8);
            *consumed = semi - start + 1;
            return true;
        }
    }
    return false;
}

// ── Emoji shortcodes ──────────────────────────────────────────────────────
// A curated table of the shortcodes LLMs actually use.  Cheap linear scan;
// size stays well under the CPU cache line count that would matter.
struct EmojiCode { std::string_view name; std::string_view utf8; };
static constexpr std::array<EmojiCode, 52> kEmojis{{
    {"smile",             "\xF0\x9F\x98\x84"},
    {"grin",              "\xF0\x9F\x98\x81"},
    {"laughing",          "\xF0\x9F\x98\x86"},
    {"joy",               "\xF0\x9F\x98\x82"},
    {"heart",             "\xE2\x9D\xA4"},
    {"broken_heart",      "\xF0\x9F\x92\x94"},
    {"thumbsup",          "\xF0\x9F\x91\x8D"},
    {"thumbsdown",        "\xF0\x9F\x91\x8E"},
    {"+1",                "\xF0\x9F\x91\x8D"},
    {"-1",                "\xF0\x9F\x91\x8E"},
    {"eyes",              "\xF0\x9F\x91\x80"},
    {"tada",              "\xF0\x9F\x8E\x89"},
    {"rocket",            "\xF0\x9F\x9A\x80"},
    {"fire",              "\xF0\x9F\x94\xA5"},
    {"bug",               "\xF0\x9F\x90\x9B"},
    {"sparkles",          "\xE2\x9C\xA8"},
    {"star",              "\xE2\xAD\x90"},
    {"warning",           "\xE2\x9A\xA0\xEF\xB8\x8F"},
    {"check",             "\xE2\x9C\x85"},
    {"white_check_mark",  "\xE2\x9C\x85"},
    {"x",                 "\xE2\x9D\x8C"},
    {"lock",              "\xF0\x9F\x94\x92"},
    {"unlock",            "\xF0\x9F\x94\x93"},
    {"key",               "\xF0\x9F\x94\x91"},
    {"bulb",              "\xF0\x9F\x92\xA1"},
    {"book",              "\xF0\x9F\x93\x96"},
    {"books",             "\xF0\x9F\x93\x9A"},
    {"memo",              "\xF0\x9F\x93\x9D"},
    {"pencil",            "\xE2\x9C\x8F\xEF\xB8\x8F"},
    {"wrench",            "\xF0\x9F\x94\xA7"},
    {"hammer",            "\xF0\x9F\x94\xA8"},
    {"zap",               "\xE2\x9A\xA1"},
    {"boom",              "\xF0\x9F\x92\xA5"},
    {"rotating_light",    "\xF0\x9F\x9A\xA8"},
    {"construction",      "\xF0\x9F\x9A\xA7"},
    {"package",           "\xF0\x9F\x93\xA6"},
    {"mag",               "\xF0\x9F\x94\x8D"},
    {"chart",             "\xF0\x9F\x93\x88"},
    {"calendar",          "\xF0\x9F\x93\x85"},
    {"clock",             "\xF0\x9F\x95\x92"},
    {"hourglass",         "\xE2\x8C\x9B"},
    {"coffee",            "\xE2\x98\x95"},
    {"thinking",          "\xF0\x9F\xA4\x94"},
    {"wave",              "\xF0\x9F\x91\x8B"},
    {"muscle",            "\xF0\x9F\x92\xAA"},
    {"ok_hand",           "\xF0\x9F\x91\x8C"},
    {"clap",              "\xF0\x9F\x91\x8F"},
    {"sunny",             "\xE2\x98\x80\xEF\xB8\x8F"},
    {"cloud",             "\xE2\x98\x81\xEF\xB8\x8F"},
    {"snowflake",         "\xE2\x9D\x84\xEF\xB8\x8F"},
    {"moon",              "\xF0\x9F\x8C\x99"},
    {"earth",             "\xF0\x9F\x8C\x8D"},
}};

static std::string_view lookup_emoji(std::string_view name) {
    for (auto& e : kEmojis)
        if (e.name == name) return e.utf8;
    return {};
}

// ── URL / mention sniffing helpers ────────────────────────────────────────

inline bool is_url_char(unsigned char c) noexcept {
    // RFC 3986 unreserved + common sub-delims used in URLs.  Stops at
    // whitespace, angle brackets, and trailing punctuation (handled by the
    // caller by shrinking past closing `.,;:)!?]`).
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '_' || c == '.' || c == '~' ||
           c == '/' || c == '?' || c == '#' || c == '=' || c == '&' ||
           c == '%' || c == '+' || c == ':' || c == '@' || c == ',' ||
           c == ';' || c == '!' || c == '(' || c == ')';
}

inline bool is_word_char(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

// Find the end of a bare URL starting at `start`.  Returns the byte offset
// one past the URL, or start if it's not a URL.  Trailing punctuation is
// stripped so "See https://x.com/a." doesn't swallow the period.
static size_t scan_bare_url(std::string_view text, size_t start) {
    size_t e = start;
    while (e < text.size() && is_url_char(static_cast<unsigned char>(text[e])))
        ++e;
    // Drop trailing punctuation like `.,;:)!?`
    while (e > start) {
        char c = text[e - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' ||
            c == '!' || c == '?' || c == ')') --e;
        else break;
    }
    return e;
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

// Parse `(url [title])` starting at `text[pos] == '('`. Consumes through the
// closing ')'.  URL may be wrapped in <...>.  Title is "..." or '...' (or
// (...)).  Returns true on success and updates pos/url/title; otherwise pos
// is unchanged.
static bool parse_link_dest_paren(std::string_view text, size_t& pos,
                                  std::string& url, std::string& title) {
    if (pos >= text.size() || text[pos] != '(') return false;
    size_t i = pos + 1;
    auto skip_ws = [&] {
        while (i < text.size() &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) ++i;
    };
    skip_ws();

    std::string u;
    if (i < text.size() && text[i] == '<') {
        ++i;
        while (i < text.size() && text[i] != '>' && text[i] != '\n') {
            if (text[i] == '\\' && i + 1 < text.size()) {
                u += text[i + 1]; i += 2; continue;
            }
            u += text[i++];
        }
        if (i >= text.size() || text[i] != '>') return false;
        ++i;
    } else {
        int depth = 0;
        while (i < text.size()) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size()) {
                u += text[i + 1]; i += 2; continue;
            }
            if (c == '(') { ++depth; u += c; ++i; continue; }
            if (c == ')') {
                if (depth == 0) break;
                --depth; u += c; ++i; continue;
            }
            if (c == ' ' || c == '\t' || c == '\n') break;
            u += c;
            ++i;
        }
    }
    skip_ws();

    std::string t;
    if (i < text.size() && (text[i] == '"' || text[i] == '\'' || text[i] == '(')) {
        char open_q = text[i];
        char close_q = (open_q == '(') ? ')' : open_q;
        ++i;
        size_t title_limit = std::min(text.size(), i + 1000);
        while (i < title_limit && text[i] != close_q) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                t += text[i + 1]; i += 2; continue;
            }
            t += text[i++];
        }
        if (i < text.size() && text[i] == close_q) ++i;
    }
    skip_ws();

    if (i >= text.size() || text[i] != ')') return false;
    url = std::move(u);
    title = std::move(t);
    pos = i + 1;
    return true;
}

// Parse `[ref]` immediately after a [text] for reference links.  Uses
// `fallback_label` when [] is empty (collapsed form).  On success, advances
// pos past ']' and returns the resolved LinkRef; otherwise returns nullptr.
static const md::LinkRef* parse_link_ref(std::string_view text, size_t& pos,
                                         std::string_view fallback_label) {
    if (pos >= text.size() || text[pos] != '[') return nullptr;
    size_t i = pos + 1;
    size_t close = std::string_view::npos;
    size_t limit = std::min(text.size(), i + 1000);
    for (size_t s = i; s < limit; ++s) {
        if (text[s] == '\\' && s + 1 < text.size()) { ++s; continue; }
        if (text[s] == ']') { close = s; break; }
        if (text[s] == '\n' || text[s] == '[') break;
    }
    if (close == std::string_view::npos) return nullptr;
    auto label = text.substr(i, close - i);
    if (label.empty()) label = fallback_label;
    auto* ref = lookup_ref(label);
    if (!ref) return nullptr;
    pos = close + 1;
    return ref;
}

// ── HTML tag parser helpers ───────────────────────────────────────────────
// We recognise a small, explicitly allow-listed subset of HTML tags and pass
// everything else through as literal text.  Two forms:
//   - void tags: <br> / <br/>  — no closer, emit a HardBreak
//   - paired tags: <kbd>..</kbd>, <mark>, <sub>, <sup>, <strong>, <em>,
//                  <span>, <abbr title="…">, <a id="…">
// Parser matches case-insensitively and allows attributes only for <abbr>
// and <a> (title and id respectively).  Unknown tags fall through so the raw
// text remains visible to the user.

struct HtmlTagInfo {
    bool        matched      = false;
    bool        is_closer    = false;
    bool        self_closing = false;
    std::string name;                   // lowercased, no <, /, >, or attrs
    std::string attr_title;             // for <abbr title="...">
    std::string attr_id;                // for <a id="...">
    std::string attr_href;              // for <a href="...">
    size_t      end          = 0;       // one past the closing '>'
};

static HtmlTagInfo try_parse_html_tag(std::string_view text, size_t start) {
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

// Find the matching closer `</tag>` for a paired HTML tag.  Bounded scan.
static size_t find_html_closer(std::string_view text, size_t start,
                               std::string_view tag, size_t max_dist = 4000) {
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

// ── Text-node post-pass: entities, emoji, bare URLs, mentions ─────────────

// Expand a single Text run into a sequence of Inline nodes, decoding HTML
// entities, replacing `:emoji:` shortcodes, linkifying bare URLs, and
// turning `@user` / `#N` / `org/repo#N` into Mention nodes.  Runs in a
// single left-to-right pass over the input string.
static std::vector<md::Inline> split_text_transform(std::string_view text) {
    std::vector<md::Inline> out;
    std::string buf;

    auto flush = [&] {
        if (!buf.empty()) {
            out.push_back(md::Text{std::move(buf)});
            buf.clear();
        }
    };

    auto word_boundary_before = [&](size_t i) -> bool {
        if (i == 0 && buf.empty()) return true;
        char prev = (i > 0) ? text[i - 1] : (buf.empty() ? '\0' : buf.back());
        return !is_word_char(static_cast<unsigned char>(prev));
    };

    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];

        // HTML entity: &name; / &#N; / &#xH;
        if (c == '&') {
            std::string decoded;
            size_t consumed = 0;
            if (try_decode_entity(text, i, decoded, &consumed)) {
                buf += decoded;
                i += consumed;
                continue;
            }
        }

        // Emoji shortcode :name:
        if (c == ':' && i + 1 < text.size()) {
            char n0 = text[i + 1];
            if (std::isalpha(static_cast<unsigned char>(n0)) ||
                n0 == '+' || n0 == '-' || n0 == '_') {
                size_t limit = std::min(text.size(), i + 40);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < limit; ++s) {
                    char x = text[s];
                    if (x == ':') { end = s; break; }
                    if (!std::isalnum(static_cast<unsigned char>(x)) &&
                        x != '_' && x != '-' && x != '+') { end = std::string_view::npos; break; }
                }
                if (end != std::string_view::npos && end > i + 1) {
                    auto name = text.substr(i + 1, end - i - 1);
                    auto utf8 = lookup_emoji(name);
                    if (!utf8.empty()) {
                        buf.append(utf8);
                        i = end + 1;
                        continue;
                    }
                }
            }
        }

        // Bare URL: http:// https:// www.  (word-boundary preceding)
        if ((c == 'h' || c == 'w' || c == 'H' || c == 'W') &&
            word_boundary_before(i)) {
            size_t prefix = 0;
            bool need_scheme = false;
            auto tail = text.substr(i);
            if (tail.size() >= 7 && (tail.substr(0, 7) == "http://" ||
                                     tail.substr(0, 7) == "HTTP://")) prefix = 7;
            else if (tail.size() >= 8 && (tail.substr(0, 8) == "https://" ||
                                          tail.substr(0, 8) == "HTTPS://")) prefix = 8;
            else if (tail.size() >= 5 && (tail.substr(0, 4) == "www." ||
                                          tail.substr(0, 4) == "WWW.") &&
                     std::isalnum(static_cast<unsigned char>(tail[4]))) {
                prefix = 4;
                need_scheme = true;
            }
            if (prefix > 0) {
                size_t end = scan_bare_url(text, i + prefix);
                if (end > i + prefix + 1) {
                    auto span = text.substr(i, end - i);
                    std::string href = need_scheme
                        ? "http://" + std::string{span}
                        : std::string{span};
                    flush();
                    out.push_back(md::Link{
                        std::string{span}, std::move(href), ""});
                    i = end;
                    continue;
                }
            }
        }

        // GitHub cross-repo ref: owner/repo#NNN
        if ((std::isalnum(static_cast<unsigned char>(c)) || c == '_') &&
            word_boundary_before(i)) {
            size_t p = i;
            while (p < text.size() && (std::isalnum(static_cast<unsigned char>(text[p])) ||
                                       text[p] == '-' || text[p] == '_' ||
                                       text[p] == '.')) ++p;
            if (p > i && p < text.size() && text[p] == '/') {
                size_t q = p + 1;
                while (q < text.size() && (std::isalnum(static_cast<unsigned char>(text[q])) ||
                                           text[q] == '-' || text[q] == '_' ||
                                           text[q] == '.')) ++q;
                if (q > p + 1 && q < text.size() && text[q] == '#') {
                    size_t r = q + 1;
                    while (r < text.size() &&
                           std::isdigit(static_cast<unsigned char>(text[r]))) ++r;
                    if (r > q + 1 && r - q - 1 <= 10) {
                        auto owner = text.substr(i,     p - i);
                        auto repo  = text.substr(p + 1, q - p - 1);
                        auto num   = text.substr(q + 1, r - q - 1);
                        std::string display{text.substr(i, r - i)};
                        std::string url = "https://github.com/";
                        url.append(owner); url += '/';
                        url.append(repo);  url += "/issues/";
                        url.append(num);
                        flush();
                        out.push_back(md::Mention{
                            md::Mention::Kind::CrossRepo, std::move(display), std::move(url)});
                        i = r;
                        continue;
                    }
                }
            }
        }

        // @user mention
        if (c == '@' && word_boundary_before(i) && i + 1 < text.size() &&
            (std::isalnum(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) {
            size_t e = i + 1;
            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) ||
                                       text[e] == '-' || text[e] == '_')) ++e;
            if (e - i >= 2 && e - i <= 40) {
                std::string display{text.substr(i, e - i)};
                std::string url = "https://github.com/";
                url.append(text.substr(i + 1, e - i - 1));
                flush();
                out.push_back(md::Mention{
                    md::Mention::Kind::User, std::move(display), std::move(url)});
                i = e;
                continue;
            }
        }

        // #NNN issue reference
        if (c == '#' && word_boundary_before(i) && i + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            size_t e = i + 1;
            while (e < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[e]))) ++e;
            if (e - i >= 2 && e - i <= 10) {
                std::string display{text.substr(i, e - i)};
                flush();
                out.push_back(md::Mention{
                    md::Mention::Kind::Issue, std::move(display), ""});
                i = e;
                continue;
            }
        }

        buf += c;
        ++i;
    }
    flush();
    return out;
}

// Apply split_text_transform to every Text node in `nodes`.  Non-Text nodes
// pass through unchanged (their children have already been transformed by
// parse_inlines recursion).  Adjacent Text runs are coalesced.
static void post_process_text_nodes(std::vector<md::Inline>& nodes) {
    std::vector<md::Inline> out;
    out.reserve(nodes.size());
    for (auto& span : nodes) {
        auto* t = std::get_if<md::Text>(&span.inner);
        if (!t) { out.push_back(std::move(span)); continue; }
        if (t->content.empty()) continue;
        // Skip expensive transform if no trigger chars present.
        bool has_trigger = false;
        for (char c : t->content) {
            if (c == '&' || c == ':' || c == '@' || c == '#' ||
                c == 'h' || c == 'w' || c == 'H' || c == 'W' || c == '/') {
                has_trigger = true; break;
            }
        }
        if (!has_trigger) { out.push_back(std::move(span)); continue; }
        auto expanded = split_text_transform(t->content);
        for (auto& e : expanded) {
            auto* et = std::get_if<md::Text>(&e.inner);
            if (et && !out.empty()) {
                if (auto* ot = std::get_if<md::Text>(&out.back().inner)) {
                    ot->content += et->content;
                    continue;
                }
            }
            out.push_back(std::move(e));
        }
    }
    nodes = std::move(out);
}

// ── Underscore intraword-flanking gates (CommonMark) ──────────────────────
// `_` delimiters cannot open or close emphasis inside a word: `foo_bar_baz`
// must NOT emphasize. `*` has no such restriction. These helpers are called
// from the hot per-byte inner loop in `parse_inlines` (also from streaming
// render_tail every frame against the live line), so they live as free
// inline functions — stamping them as lambdas inside the loop body cost a
// measurable hit on long replies. Pure functions of (text, position); no
// state, no allocation.
[[nodiscard]] static inline bool is_word_byte(char c) noexcept {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || uc == '_';
}
[[nodiscard]] static inline bool underscore_open_ok(std::string_view text,
                                                    size_t open_at) noexcept {
    // `*` always allowed; `_` only at a word boundary on the left.
    if (text[open_at] != '_') return true;
    if (open_at == 0) return true;
    return !is_word_byte(text[open_at - 1]);
}
[[nodiscard]] static inline bool underscore_close_ok(std::string_view text,
                                                     char delim_ch,
                                                     size_t after_close) noexcept {
    // For a `_` run the byte just past the closing delimiter must not be
    // word-class. `*` always allowed.
    if (delim_ch != '_') return true;
    if (after_close >= text.size()) return true;
    return !is_word_byte(text[after_close]);
}

std::vector<md::Inline> parse_inlines(std::string_view text) {
    std::vector<md::Inline> result;
    size_t i = 0;

    while (i < text.size()) {
        // Backslash escape
        if (text[i] == '\\') {
            if (i + 1 < text.size() && is_escapable(text[i + 1])) {
                push_char(result, text[i + 1]);
                i += 2;
                continue;
            }
            // Hard line break: backslash before newline
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                result.push_back(md::HardBreak{});
                i += 2;
                continue;
            }
            // Trailing or unrecognized backslash — emit as a literal so we
            // always advance.  Without this, `\` appears in kInlineSpecial
            // but never gets consumed, causing the outer loop to spin
            // forever (e.g. input ending in "\\" or "\\<space>").
            push_text(result, text.substr(i, 1));
            ++i;
            continue;
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
        // CommonMark: '_' delimiters do not open/close inside a word —
        // `foo_bar_baz` must NOT emphasize. `*` has no such restriction.
        // The underscore-flanking gates live as free functions just
        // above this function (see `underscore_open_ok` /
        // `underscore_close_ok`) so the lambda-construction cost
        // doesn't land on every iteration of this hot loop — the
        // render_tail path re-parses the live line on every frame
        // and lambda churn was visible as a measurable per-keystroke
        // slowdown on long streaming replies.

        if (i + 2 < text.size() &&
            ((text[i] == '*' && text[i+1] == '*' && text[i+2] == '*') ||
             (text[i] == '_' && text[i+1] == '_' && text[i+2] == '_'))) {
            auto delim = text.substr(i, 3);
            char dc = text[i];
            if (underscore_open_ok(text, i)) {
                size_t end = find_closing(text, delim, i + 3);
                if (end != std::string_view::npos &&
                    underscore_close_ok(text, dc, end + 3)) {
                    auto inner = text.substr(i + 3, end - i - 3);
                    result.push_back(md::BoldItalic{parse_inlines(inner)});
                    i = end + 3;
                    continue;
                }
            }
        }

        // Bold: **text** or __text__
        if (i + 1 < text.size() &&
            ((text[i] == '*' && text[i + 1] == '*') ||
             (text[i] == '_' && text[i + 1] == '_'))) {
            auto delim = text.substr(i, 2);
            char dc = text[i];
            if (underscore_open_ok(text, i)) {
                size_t end = find_closing(text, delim, i + 2);
                if (end != std::string_view::npos &&
                    underscore_close_ok(text, dc, end + 2)) {
                    auto inner = text.substr(i + 2, end - i - 2);
                    result.push_back(md::Bold{parse_inlines(inner)});
                    i = end + 2;
                    continue;
                }
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

        // Subscript: ~text~ (single ~) — must come after strike (~~) check.
        // Bounded scan; require non-space flanking to avoid matching prose
        // like "before~after" in URLs (already handled earlier in autolinks).
        if (text[i] == '~') {
            if (i + 1 < text.size() && text[i + 1] != '~' && text[i + 1] != ' ') {
                size_t scan_limit = std::min(text.size(), i + 1 + 200);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < scan_limit; ++s) {
                    if (text[s] == '~') { end = s; break; }
                    // Subscripts don't span spaces or newlines.
                    if (text[s] == ' ' || text[s] == '\n') break;
                }
                if (end != std::string_view::npos && text[end - 1] != ' ') {
                    auto inner = text.substr(i + 1, end - i - 1);
                    result.push_back(md::Sub{parse_inlines(inner)});
                    i = end + 1;
                    continue;
                }
            }
            // No match — fall through to "unmatched ~" handler below.
        }

        // Highlight: ==text== (CommonMark extension / PHP Markdown Extra).
        if (i + 1 < text.size() && text[i] == '=' && text[i + 1] == '=') {
            size_t end = find_closing(text, "==", i + 2);
            if (end != std::string_view::npos) {
                auto inner = text.substr(i + 2, end - i - 2);
                result.push_back(md::Highlight{parse_inlines(inner)});
                i = end + 2;
                continue;
            }
            // Fall through — literal '=' may just be prose.
            push_text(result, text.substr(i, 1));
            ++i;
            continue;
        }
        if (text[i] == '=') {
            push_text(result, text.substr(i, 1));
            ++i;
            continue;
        }

        // Superscript: ^text^ — bounded, no-space, no-newline.
        if (text[i] == '^') {
            if (i + 1 < text.size() && text[i + 1] != '^' && text[i + 1] != ' ') {
                size_t scan_limit = std::min(text.size(), i + 1 + 200);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < scan_limit; ++s) {
                    if (text[s] == '^') { end = s; break; }
                    if (text[s] == ' ' || text[s] == '\n') break;
                }
                if (end != std::string_view::npos && text[end - 1] != ' ') {
                    auto inner = text.substr(i + 1, end - i - 1);
                    result.push_back(md::Sup{parse_inlines(inner)});
                    i = end + 1;
                    continue;
                }
            }
            push_text(result, text.substr(i, 1));
            ++i;
            continue;
        }

        // Italic: *text* or _text_ (single delimiter)
        if (text[i] == '*' || text[i] == '_') {
            char delim_ch = text[i];
            if (i + 1 < text.size() && text[i + 1] != delim_ch && text[i + 1] != ' '
                && underscore_open_ok(text, i)) {
                // Bounded scan to avoid O(n²) on many unmatched delimiters
                size_t scan_limit = std::min(text.size(), i + 1 + 2000);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < scan_limit; ++s) {
                    if (text[s] == delim_ch) { end = s; break; }
                }
                if (end != std::string_view::npos && text[end - 1] != ' '
                    && underscore_close_ok(text, delim_ch, end + 1)) {
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

        // Image: ![alt](url [title]) or ![alt][ref] or ![alt][] or ![alt]
        if (text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[') {
            size_t bracket_limit = std::min(text.size(), i + 2 + 2000);
            size_t close_bracket = std::string_view::npos;
            for (size_t s = i + 2; s < bracket_limit; ++s) {
                if (text[s] == '\\' && s + 1 < text.size()) { ++s; continue; }
                if (text[s] == ']') { close_bracket = s; break; }
            }
            if (close_bracket != std::string_view::npos) {
                auto alt = text.substr(i + 2, close_bracket - i - 2);
                size_t after = close_bracket + 1;
                if (after < text.size() && text[after] == '(') {
                    std::string url, title;
                    size_t pos = after;
                    if (parse_link_dest_paren(text, pos, url, title)) {
                        result.push_back(md::Image{
                            std::string{alt}, std::move(url), std::move(title)});
                        i = pos;
                        continue;
                    }
                } else if (after < text.size() && text[after] == '[') {
                    size_t pos = after;
                    auto* ref = parse_link_ref(text, pos, alt);
                    if (ref) {
                        result.push_back(md::Image{
                            std::string{alt}, ref->url, ref->title});
                        i = pos;
                        continue;
                    }
                } else {
                    // Shortcut ![alt] — look up `alt` directly.
                    if (auto* ref = lookup_ref(alt)) {
                        result.push_back(md::Image{
                            std::string{alt}, ref->url, ref->title});
                        i = close_bracket + 1;
                        continue;
                    }
                }
            }
            push_text(result, "!");
            ++i;
            continue;
        }

        // Link: [text](url [title]) or [text][ref] or [text][] or [text]
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
                    if (close + 1 >= text.size() || text[close + 1] != '(') {
                        result.push_back(md::FootnoteRef{std::string{label}});
                        i = close + 1;
                        continue;
                    }
                }
            }

            // Find matching ']' — links can contain balanced [] brackets
            // for clickable-image syntax [![alt](img)](url).  One level of
            // nesting is enough to cover the common case.
            size_t bracket_limit = std::min(text.size(), i + 1 + 2000);
            size_t close_bracket = std::string_view::npos;
            int depth = 0;
            for (size_t s = i + 1; s < bracket_limit; ++s) {
                if (text[s] == '\\' && s + 1 < text.size()) { ++s; continue; }
                if (text[s] == '[') { ++depth; continue; }
                if (text[s] == ']') {
                    if (depth == 0) { close_bracket = s; break; }
                    --depth;
                }
            }
            if (close_bracket != std::string_view::npos) {
                auto link_text = text.substr(i + 1, close_bracket - i - 1);
                size_t after = close_bracket + 1;
                if (after < text.size() && text[after] == '(') {
                    std::string url, title;
                    size_t pos = after;
                    if (parse_link_dest_paren(text, pos, url, title)) {
                        result.push_back(md::Link{
                            std::string{link_text}, std::move(url), std::move(title)});
                        i = pos;
                        continue;
                    }
                } else if (after < text.size() && text[after] == '[') {
                    size_t pos = after;
                    if (auto* ref = parse_link_ref(text, pos, link_text)) {
                        result.push_back(md::Link{
                            std::string{link_text}, ref->url, ref->title});
                        i = pos;
                        continue;
                    }
                } else {
                    if (auto* ref = lookup_ref(link_text)) {
                        result.push_back(md::Link{
                            std::string{link_text}, ref->url, ref->title});
                        i = close_bracket + 1;
                        continue;
                    }
                }
            }
            push_text(result, "[");
            ++i;
            continue;
        }

        // `<` — autolink, HTML entity, HTML comment, or HTML tag.
        if (text[i] == '<') {
            // HTML comment `<!-- ... -->` — render nothing (author notes,
            // TODOs, linter pragmas are invisible in final output, matching
            // CommonMark's behavior).  Bounded scan to cap worst-case.
            if (i + 3 < text.size() &&
                text[i + 1] == '!' && text[i + 2] == '-' && text[i + 3] == '-') {
                size_t limit = std::min(text.size(), i + 4000);
                size_t j = i + 4;
                bool closed = false;
                while (j + 2 < limit) {
                    if (text[j] == '-' && text[j + 1] == '-' && text[j + 2] == '>') {
                        i = j + 3; closed = true; break;
                    }
                    ++j;
                }
                if (closed) continue;
                // Unterminated: fall through and emit the '<' as text.
            }
            // Try HTML tag first (allow-listed subset).
            auto tag = try_parse_html_tag(text, i);
            if (tag.matched) {
                // <br> / <br/>  → HardBreak
                if (tag.name == "br") {
                    result.push_back(md::HardBreak{});
                    i = tag.end;
                    continue;
                }
                // <a id="…"> anchor marker — emit nothing visible.
                if (tag.name == "a" && !tag.is_closer) {
                    if (tag.self_closing) {
                        i = tag.end; continue;
                    }
                    // <a id="x">text</a> — render text styled as a link
                    // only when href is present; otherwise treat as plain.
                    size_t closer = find_html_closer(text, tag.end, "a");
                    auto body = (closer == std::string_view::npos)
                        ? text.substr(tag.end)
                        : text.substr(tag.end, closer - tag.end);
                    auto inner = parse_inlines(body);
                    if (!tag.attr_href.empty()) {
                        std::string tx;
                        for (auto& sp : inner)
                            if (auto* t = std::get_if<md::Text>(&sp.inner)) tx += t->content;
                        result.push_back(md::Link{
                            std::move(tx), std::move(tag.attr_href), ""});
                    } else {
                        for (auto& sp : inner) result.push_back(std::move(sp));
                    }
                    i = (closer == std::string_view::npos)
                        ? text.size() : closer + 4; // len("</a>")
                    continue;
                }
                // Paired HTML tags we render.
                auto render_paired = [&](const char* name,
                                         auto wrap) -> bool {
                    if (tag.is_closer || tag.self_closing) return false;
                    if (tag.name != name) return false;
                    size_t closer = find_html_closer(text, tag.end, name);
                    auto body = (closer == std::string_view::npos)
                        ? text.substr(tag.end)
                        : text.substr(tag.end, closer - tag.end);
                    auto inner = parse_inlines(body);
                    wrap(std::move(inner), tag);
                    i = (closer == std::string_view::npos)
                        ? text.size()
                        : closer + 3 + std::string_view(name).size();
                    return true;
                };
                if (render_paired("strong", [&](auto inner, auto&) {
                    result.push_back(md::Bold{std::move(inner)});
                })) continue;
                if (render_paired("em", [&](auto inner, auto&) {
                    result.push_back(md::Italic{std::move(inner)});
                })) continue;
                if (render_paired("mark", [&](auto inner, auto&) {
                    result.push_back(md::Highlight{std::move(inner)});
                })) continue;
                if (render_paired("sub", [&](auto inner, auto&) {
                    result.push_back(md::Sub{std::move(inner)});
                })) continue;
                if (render_paired("sup", [&](auto inner, auto&) {
                    result.push_back(md::Sup{std::move(inner)});
                })) continue;
                if (render_paired("kbd", [&](auto inner, auto&) {
                    result.push_back(md::Kbd{std::move(inner)});
                })) continue;
                if (render_paired("span", [&](auto inner, auto&) {
                    for (auto& sp : inner) result.push_back(std::move(sp));
                })) continue;
                if (render_paired("abbr", [&](auto inner, auto& t) {
                    result.push_back(md::Abbr{std::move(t.attr_title), std::move(inner)});
                })) continue;
                // Unrecognized tag — fall through and treat as text.
            }

            // Autolink: <url> or <email>
            size_t close = text.find('>', i + 1);
            if (close != std::string_view::npos && close - i <= 500) {
                auto content = text.substr(i + 1, close - i - 1);
                bool has_space = content.find(' ') != std::string_view::npos ||
                                 content.find('\n') != std::string_view::npos;
                bool is_url = content.find("://") != std::string_view::npos;
                bool is_email = content.find('@') != std::string_view::npos &&
                                !has_space && !is_url;
                if ((is_url || is_email) && !has_space) {
                    std::string url_str{content};
                    if (is_email && !starts_with(content, "mailto:"))
                        url_str = "mailto:" + url_str;
                    result.push_back(md::Link{
                        std::string{content}, std::move(url_str), ""});
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

    post_process_text_nodes(result);
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

std::vector<std::string_view> split_table_cells(std::string_view line);  // fwd decl

// Parse the GFM delimiter row (`|:--|:-:|--:|`) into a per-column
// alignment list. Cells whose trimmed content starts with `:` are
// left-anchored on the left side; cells whose trimmed content ends
// with `:` are right-anchored. Both → Center. Neither → Left
// (CommonMark default for tables). The caller has already verified
// `is_table_separator(line)` is true.
std::vector<md::TableAlign> parse_table_alignments(std::string_view line) {
    std::vector<md::TableAlign> out;
    auto cells = split_table_cells(line);
    out.reserve(cells.size());
    for (auto& c : cells) {
        auto t = trim(c);
        bool left  = !t.empty() && t.front() == ':';
        bool right = !t.empty() && t.back()  == ':';
        if (left && right)      out.push_back(md::TableAlign::Center);
        else if (right)         out.push_back(md::TableAlign::Right);
        else                    out.push_back(md::TableAlign::Left);
    }
    return out;
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

// Normalize a reference label: lowercase + collapse whitespace runs.
static std::string normalize_ref_label(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    bool prev_space = false;
    for (char c : label) {
        unsigned char uc = static_cast<unsigned char>(c);
        bool sp = std::isspace(uc);
        if (sp) {
            if (!prev_space && !out.empty()) out += ' ';
            prev_space = true;
        } else {
            out += static_cast<char>(std::tolower(uc));
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Extract reference-link definitions `[label]: url "title"` from source.
// Writes them to `defs` and returns a copy of `source` with the matched
// lines removed (replaced by blank lines so line-oriented parsers still see
// block boundaries).  Lines that look like footnote defs (`[^label]: …`)
// are NOT treated as reference defs.
static std::string collect_ref_defs(std::string_view source,
                                    std::unordered_map<std::string, md::LinkRef>& defs) {
    // Early-out: a `[label]: url` line REQUIRES a literal '[' somewhere
    // in the source. The committed prose chunks coming from the
    // streaming markdown widget are most-of-the-time bracket-free
    // paragraphs; the per-line trim + ascii_lower + label parse below
    // is wasted work on those. memchr-style search for '[' is O(n) but
    // SIMD-friendly and bypasses every line break / trim that the loop
    // otherwise has to do. When there's no '[' we can skip both passes:
    // no defs to collect, and the returned string is the source verbatim.
    if (source.find('[') == std::string_view::npos) {
        return std::string{source};
    }

    std::string out;
    out.reserve(source.size());

    size_t i = 0;
    while (i < source.size()) {
        size_t nl = source.find('\n', i);
        size_t line_end = (nl == std::string_view::npos) ? source.size() : nl;
        auto line = source.substr(i, line_end - i);
        auto trimmed = trim(line);

        bool consumed = false;
        if (trimmed.size() >= 4 && trimmed[0] == '[' && trimmed[1] != '^') {
            size_t close = trimmed.find("]:");
            if (close != std::string_view::npos && close > 1) {
                auto label_sv = trimmed.substr(1, close - 1);
                auto rest = trim(trimmed.substr(close + 2));
                std::string url;
                size_t p = 0;
                if (!rest.empty() && rest[0] == '<') {
                    size_t gt = rest.find('>', 1);
                    if (gt != std::string_view::npos) {
                        url = std::string{rest.substr(1, gt - 1)};
                        p = gt + 1;
                    }
                } else {
                    size_t e = 0;
                    while (e < rest.size() &&
                           !std::isspace(static_cast<unsigned char>(rest[e]))) ++e;
                    url = std::string{rest.substr(0, e)};
                    p = e;
                }
                if (!url.empty()) {
                    while (p < rest.size() &&
                           std::isspace(static_cast<unsigned char>(rest[p]))) ++p;
                    std::string title;
                    if (p < rest.size() &&
                        (rest[p] == '"' || rest[p] == '\'' || rest[p] == '(')) {
                        char open_q = rest[p];
                        char close_q = (open_q == '(') ? ')' : open_q;
                        ++p;
                        while (p < rest.size() && rest[p] != close_q) {
                            title += rest[p++];
                        }
                    }
                    auto key = normalize_ref_label(label_sv);
                    if (!key.empty()) {
                        defs.emplace(std::move(key),
                                     md::LinkRef{std::move(url), std::move(title)});
                        consumed = true;
                    }
                }
            }
        }

        if (!consumed) {
            out.append(line);
        }
        if (nl != std::string_view::npos) out += '\n';

        if (nl == std::string_view::npos) break;
        i = nl + 1;
    }
    return out;
}

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

        // HTML comment block: `<!-- ... -->` (possibly multi-line).
        // Render nothing — TODOs / linter pragmas / rendering hints stay
        // invisible. If the open is mid-paragraph, let paragraph parsing
        // handle it so inline comment stripping kicks in.
        if (paragraph_buf.empty() && starts_with(trimmed, "<!--")) {
            // Terminating --> on the same line?
            if (auto end = trimmed.find("-->", 4); end != std::string_view::npos) {
                auto rest = trim(trimmed.substr(end + 3));
                if (rest.empty()) { ++i; continue; }
                // Tail text after --> falls through to normal processing.
            } else {
                // Multi-line: consume until a line containing -->
                ++i;
                while (i < lines.size()) {
                    auto cl = lines[i];
                    if (!cl.empty() && cl.back() == '\r') cl.remove_suffix(1);
                    ++i;
                    if (cl.find("-->") != std::string_view::npos) break;
                }
                continue;
            }
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

        // Blockquote (or GitHub alert: `> [!NOTE] …`)
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
            // GitHub alert detection: first non-blank line is one of
            // `[!NOTE]` / `[!TIP]` / `[!IMPORTANT]` / `[!WARNING]` /
            // `[!CAUTION]` (case-insensitive).
            //
            // Why "first non-blank" rather than "first": during
            // streaming, blockquote content can arrive as
            //   > 
            //   > [!NOTE]
            //   > body
            // where the very first `>` line carries an empty payload
            // (the model emitted `> ` then a newline before the alert
            // tag). The original check only looked at line 0 of
            // bq_text and missed those cases — the alert silently
            // degraded to a plain blockquote depending on chunk
            // boundaries.
            std::size_t alert_line_start = 0;
            while (alert_line_start < bq_text.size()) {
                std::size_t eol = bq_text.find('\n', alert_line_start);
                std::size_t end = (eol == std::string::npos)
                                  ? bq_text.size() : eol;
                auto cand = std::string_view{bq_text}
                                .substr(alert_line_start,
                                        end - alert_line_start);
                if (!trim(cand).empty()) break;
                if (eol == std::string::npos) {
                    alert_line_start = bq_text.size();
                    break;
                }
                alert_line_start = eol + 1;
            }
            std::size_t alert_eol = bq_text.find('\n', alert_line_start);
            auto first_line = (alert_eol == std::string::npos)
                ? std::string_view{bq_text}.substr(alert_line_start)
                : std::string_view{bq_text}.substr(
                      alert_line_start,
                      alert_eol - alert_line_start);
            auto ft = trim(first_line);
            if (ft.size() > 3 && ft.front() == '[' && ft[1] == '!') {
                auto close_b = ft.find(']');
                if (close_b != std::string_view::npos && close_b > 2) {
                    auto tag = ascii_lower(ft.substr(2, close_b - 2));
                    std::optional<md::Alert::Kind> kind;
                    if      (tag == "note")      kind = md::Alert::Kind::Note;
                    else if (tag == "tip")       kind = md::Alert::Kind::Tip;
                    else if (tag == "important") kind = md::Alert::Kind::Important;
                    else if (tag == "warning")   kind = md::Alert::Kind::Warning;
                    else if (tag == "caution")   kind = md::Alert::Kind::Caution;
                    if (kind.has_value()) {
                        auto rest = (alert_eol == std::string::npos)
                            ? std::string_view{}
                            : std::string_view{bq_text}.substr(alert_eol + 1);
                        auto after_tag = ft.substr(close_b + 1);
                        std::string body;
                        body.reserve(after_tag.size() + rest.size() + 1);
                        after_tag = trim(after_tag);
                        if (!after_tag.empty()) body.append(after_tag);
                        if (!rest.empty()) {
                            if (!body.empty()) body += '\n';
                            body.append(rest);
                        }
                        auto inner = parse_markdown_impl(body, depth + 1);
                        doc.blocks.push_back(md::Alert{
                            *kind, std::move(inner.blocks)});
                        continue;
                    }
                }
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
            std::string_view delim_line;
            if (i + 1 < lines.size()) {
                delim_line = lines[i + 1];
                if (!delim_line.empty() && delim_line.back() == '\r')
                    delim_line.remove_suffix(1);
                is_table = is_table_separator(delim_line);
            }
            if (is_table) {
                flush_paragraph();
                auto header_cells = split_table_cells(line);
                md::TableRow header;
                for (auto& cell : header_cells) {
                    header.cells.push_back(md::TableCell{parse_inlines(cell)});
                }
                auto aligns = parse_table_alignments(delim_line);
                // Pad / truncate aligns to match the header column count
                // so render.cpp can index by column without bounds checks.
                aligns.resize(header.cells.size(), md::TableAlign::Left);
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
                doc.blocks.push_back(md::Table{
                    std::move(header), std::move(rows), std::move(aligns)});
                continue;
            }
        }

        // Definition-list item: paragraph followed by one or more `: def` lines.
        //   term1
        //   : definition 1
        //   : definition 2
        //
        // We detect this when paragraph_buf has exactly one non-empty line
        // (the term) and the current line begins with `: `.
        if (!paragraph_buf.empty() && trimmed.size() >= 2 &&
            trimmed[0] == ':' && (trimmed[1] == ' ' || trimmed[1] == '\t')) {
            // Only if paragraph_buf holds a single-line term (no embedded
            // newlines) — otherwise treat `:` as regular prose.
            if (paragraph_buf.find('\n') == std::string::npos) {
                // Snapshot the term BEFORE clearing paragraph_buf —
                // `trim()` returns a string_view aliasing paragraph_buf's
                // storage, and `paragraph_buf.clear()` writes a '\0'
                // terminator at index 0 (libstdc++ / libc++ both do this
                // even though the capacity is preserved). Without the
                // copy, parse_inlines would read the just-zeroed first
                // byte and emit a Text node whose first character is
                // '\0' — visible as the term losing its first letter
                // ("term1" → "erm1").
                std::string term_text{trim(paragraph_buf)};
                paragraph_buf.clear();

                md::DefItem item;
                item.term = parse_inlines(term_text);

                while (i < lines.size()) {
                    auto dl = lines[i];
                    if (!dl.empty() && dl.back() == '\r') dl.remove_suffix(1);
                    auto dt = trim(dl);
                    if (dt.size() < 2 || dt[0] != ':' ||
                        (dt[1] != ' ' && dt[1] != '\t')) break;
                    std::string def{dt.substr(2)};
                    ++i;
                    // Continuation lines: indented by 2+ spaces.
                    while (i < lines.size()) {
                        auto cl = lines[i];
                        if (!cl.empty() && cl.back() == '\r') cl.remove_suffix(1);
                        if (trim(cl).empty()) break;
                        if (count_indent(cl) < 2) break;
                        def += '\n';
                        def += dedent(cl, 2);
                        ++i;
                    }
                    auto sub = parse_markdown_impl(def, depth + 1);
                    item.defs.push_back(std::move(sub.blocks));
                    // Allow blank line between defs.
                    while (i < lines.size() && trim(lines[i]).empty()) ++i;
                }
                // Collect subsequent terms into the same list.
                md::DefList dl_block;
                dl_block.items.push_back(std::move(item));
                while (i < lines.size()) {
                    auto nl = lines[i];
                    if (!nl.empty() && nl.back() == '\r') nl.remove_suffix(1);
                    auto nt = trim(nl);
                    if (nt.empty()) break;
                    // A term is a line NOT starting with `:` and followed by
                    // another `: …` line.
                    if (nt[0] == ':' || nt[0] == '#' || nt[0] == '>' ||
                        nt[0] == '|' || nt[0] == '-' || nt[0] == '*' ||
                        nt[0] == '+' || nt[0] == '`') break;
                    if (i + 1 >= lines.size()) break;
                    auto peek = trim(lines[i + 1]);
                    if (peek.size() < 2 || peek[0] != ':' ||
                        (peek[1] != ' ' && peek[1] != '\t')) break;

                    md::DefItem next;
                    next.term = parse_inlines(nt);
                    i += 1;
                    while (i < lines.size()) {
                        auto dl = lines[i];
                        if (!dl.empty() && dl.back() == '\r') dl.remove_suffix(1);
                        auto dt = trim(dl);
                        if (dt.size() < 2 || dt[0] != ':' ||
                            (dt[1] != ' ' && dt[1] != '\t')) break;
                        std::string def{dt.substr(2)};
                        ++i;
                        while (i < lines.size()) {
                            auto cl = lines[i];
                            if (!cl.empty() && cl.back() == '\r') cl.remove_suffix(1);
                            if (trim(cl).empty()) break;
                            if (count_indent(cl) < 2) break;
                            def += '\n';
                            def += dedent(cl, 2);
                            ++i;
                        }
                        auto sub = parse_markdown_impl(def, depth + 1);
                        next.defs.push_back(std::move(sub.blocks));
                        while (i < lines.size() && trim(lines[i]).empty()) ++i;
                    }
                    dl_block.items.push_back(std::move(next));
                }
                doc.blocks.push_back(std::move(dl_block));
                continue;
            }
        }

        // <details><summary>…</summary>…</details> — render as a titled
        // blockquote.  Any other recognized HTML block (or unrecognized
        // block-level tag) falls through to HtmlBlock.
        if (!trimmed.empty() && trimmed[0] == '<' && paragraph_buf.empty()) {
            auto open = try_parse_html_tag(trimmed, 0);
            if (open.matched && !open.is_closer) {
                if (open.name == "details") {
                    flush_paragraph();
                    // Collect body until </details> at line start.
                    std::string body;
                    // Remainder of this line after the <details> tag.
                    auto tail = trimmed.substr(open.end);
                    if (!tail.empty()) body.append(tail);
                    ++i;
                    bool found_close = false;
                    while (i < lines.size()) {
                        auto dl = lines[i];
                        if (!dl.empty() && dl.back() == '\r') dl.remove_suffix(1);
                        auto dt = trim(dl);
                        if (starts_with(ascii_lower(std::string{dt}), "</details>")) {
                            ++i;
                            found_close = true;
                            break;
                        }
                        if (!body.empty()) body += '\n';
                        body += dl;
                        ++i;
                    }
                    (void)found_close;
                    // Extract <summary>…</summary> from body.
                    std::vector<md::Inline> summary;
                    auto body_lc = ascii_lower(body);
                    size_t sm_open  = body_lc.find("<summary");
                    if (sm_open != std::string::npos) {
                        size_t sm_gt = body.find('>', sm_open);
                        size_t sm_close = body_lc.find("</summary>", sm_gt + 1);
                        if (sm_gt != std::string::npos &&
                            sm_close != std::string::npos) {
                            auto sum_text = std::string_view{body}
                                .substr(sm_gt + 1, sm_close - sm_gt - 1);
                            summary = parse_inlines(sum_text);
                            // Strip the <summary>…</summary> from body.
                            body.erase(sm_open, sm_close + 10 - sm_open);
                        }
                    }
                    auto inner = parse_markdown_impl(body, depth + 1);
                    doc.blocks.push_back(md::Details{
                        std::move(summary), std::move(inner.blocks)});
                    continue;
                }
                // Unrecognized block-level HTML — consume lines until a
                // matching closer (case-insensitive) or a blank line.
                // Only treat as HtmlBlock for a conservative allowlist;
                // otherwise let it fall through to paragraph parsing so
                // inline-level tags (<strong>, <kbd>, …) still work.
                static constexpr std::array<std::string_view, 14> kBlockTags{{
                    "div", "table", "ul", "ol", "dl", "p", "pre",
                    "blockquote", "form", "figure", "section", "article",
                    "aside", "nav",
                }};
                bool is_block = false;
                for (auto& t : kBlockTags) if (open.name == t) { is_block = true; break; }
                if (is_block) {
                    flush_paragraph();
                    std::string content{line};
                    ++i;
                    while (i < lines.size()) {
                        auto hl = lines[i];
                        if (!hl.empty() && hl.back() == '\r') hl.remove_suffix(1);
                        if (trim(hl).empty()) { ++i; break; }
                        content += '\n';
                        content += hl;
                        auto lc = ascii_lower(std::string{trim(hl)});
                        if (starts_with(lc, "</" + open.name + ">")) {
                            ++i;
                            break;
                        }
                        ++i;
                    }
                    doc.blocks.push_back(md::HtmlBlock{std::move(content)});
                    continue;
                }
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
    md::Document doc;
    // Pre-pass: pull out `[label]: url "title"` definitions and remember
    // them in the Document, because inline parsing needs the map to resolve
    // references that appear *before* the definition textually.
    std::string cleaned = collect_ref_defs(source, doc.ref_defs);

    RefDefsGuard guard(&doc.ref_defs);
    auto parsed = parse_markdown_impl(cleaned, 0);
    doc.blocks = std::move(parsed.blocks);
    return doc;
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
std::vector<md::Inline> parse_inlines(std::string_view text) {
    return ::maya::parse_inlines(text);
}
std::size_t find_eol(const char* data, std::size_t start, std::size_t end) noexcept {
    return ::maya::find_eol(data, start, end);
}
std::string collect_ref_defs(std::string_view source,
                             std::unordered_map<std::string, md::LinkRef>& defs) {
    return ::maya::collect_ref_defs(source, defs);
}
md::Document parse_markdown_impl(std::string_view source, int depth) {
    return ::maya::parse_markdown_impl(source, depth);
}
// (ul_marker_len / ol_marker_len / count_indent live in internal.hpp
//  as inline functions; no thunk needed.)
} // namespace md_detail

} // namespace maya
