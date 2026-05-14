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

// ── Reference-link map (threaded through parse via thread_local) ──────────
// parse_inlines is called from many nested places (emphasis recursion, table
// cells, list items, blockquote bodies).  Threading a pointer through every
// call site would touch the whole inline parser API.  Instead, the top-level
// parse_markdown stashes the active Document's ref_defs in this pointer.
thread_local const std::unordered_map<std::string, md::LinkRef>*
    tls_ref_defs = nullptr;

struct RefDefsGuard {
    const std::unordered_map<std::string, md::LinkRef>* prev;
    explicit RefDefsGuard(const std::unordered_map<std::string, md::LinkRef>* p) noexcept
        : prev(tls_ref_defs) { tls_ref_defs = p; }
    ~RefDefsGuard() { tls_ref_defs = prev; }
    RefDefsGuard(const RefDefsGuard&) = delete;
    RefDefsGuard& operator=(const RefDefsGuard&) = delete;
};

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
            // GitHub alert detection: first line is `[!NOTE]` / `[!TIP]` /
            // `[!IMPORTANT]` / `[!WARNING]` / `[!CAUTION]` (case-insensitive).
            auto first_nl = bq_text.find('\n');
            auto first_line = (first_nl == std::string::npos)
                ? std::string_view{bq_text}
                : std::string_view{bq_text}.substr(0, first_nl);
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
                        auto rest = (first_nl == std::string::npos)
                            ? std::string_view{}
                            : std::string_view{bq_text}.substr(first_nl + 1);
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
                auto term_text = trim(paragraph_buf);
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

// ============================================================================
// AST to Element conversion — polished terminal rendering
// ============================================================================

// Terminal-adaptive color palette — uses named ANSI colors so the rendering
// automatically matches whatever terminal theme the user has configured
// (Catppuccin, Dracula, Solarized, One Dark, Gruvbox, etc.)
namespace colors {
    // Prose body text uses ANSI 15 ("bright_white") rather than ANSI 7
    // ("white"). The latter renders as a mid-gray in most popular dark
    // themes (Catppuccin / One Dark / Dracula / Gruvbox / Solarized dark)
    // which hurts long-form readability — the eye has to work harder per
    // character. Bright_white pins prose to the terminal's highest-
    // contrast foreground so the body always reads clearly regardless of
    // theme. Dim / muted tones handle the typographic hierarchy instead
    // of lowering the base contrast.
    constexpr auto text        = Color::bright_white();
    constexpr auto heading1    = Color::bright_white();
    constexpr auto heading2    = Color::bright_white();
    constexpr auto heading3    = Color::bright_white();
    constexpr auto heading_dim  = Color::bright_black();
    constexpr auto heading_rule = Color::bright_black();
    constexpr auto bold_fg     = Color::bright_white();
    constexpr auto italic_fg   = Color::bright_white();
    constexpr auto code_fg     = Color::bright_yellow();
    constexpr auto code_bg     = Color::black();
    constexpr auto link_fg     = Color::bright_blue();
    constexpr auto image_fg    = Color::bright_magenta();
    constexpr auto strike_fg   = Color::bright_black();
    constexpr auto quote_bar   = Color::bright_black();
    constexpr auto quote_text  = Color::bright_white();
    constexpr auto list_bullet = Color::bright_black();
    constexpr auto list_num    = Color::bright_white();
    constexpr auto checkbox_fg = Color::bright_green();
    constexpr auto checkbox_off= Color::bright_black();
    constexpr auto code_border = Color::bright_black();
    constexpr auto code_lang   = Color::bright_black();
    constexpr auto hrule_fg    = Color::bright_black();
    constexpr auto footnote_fg = Color::bright_black();
    constexpr auto table_border= Color::bright_black();
    constexpr auto table_header= Color::bright_white();
    constexpr auto highlight_bg= Color::yellow();
    constexpr auto highlight_fg= Color::black();
    constexpr auto mention_fg  = Color::bright_cyan();
    constexpr auto kbd_fg      = Color::bright_white();
    constexpr auto kbd_border  = Color::bright_black();
    constexpr auto alert_note      = Color::bright_blue();
    constexpr auto alert_tip       = Color::bright_green();
    constexpr auto alert_important = Color::bright_magenta();
    constexpr auto alert_warning   = Color::bright_yellow();
    constexpr auto alert_caution   = Color::bright_red();
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
    // Gutter line-number column for code blocks ≥ 5 lines. dim +
    // bright_black so it sits visually behind the code without
    // competing with the syntax-highlighted body.
    inline const Style& gutter()   { static const Style s = Style{}.with_fg(Color::bright_black()).with_dim(); return s; }

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

    // ── Gutter pass: prepend a right-aligned line-number column to each
    //    line. Height of the rendered block is unchanged — exactly one
    //    line out per line in, so monotonicity is preserved (a code
    //    fence that committed at K rows still commits at K rows).
    //
    //    Skip on small blocks (< 5 lines): the gutter is visual noise
    //    on a 2-line snippet, where the line numbers are obvious from
    //    position alone. The threshold matches what users intuitively
    //    expect — a "code listing" rather than a "snippet".
    {
        auto count_lines = [](std::string_view s) -> int {
            int n = 0;
            for (char c : s) if (c == '\n') ++n;
            if (!s.empty() && s.back() != '\n') ++n;
            return n;
        };
        const int line_count = count_lines(out);
        constexpr int kGutterMinLines = 5;
        if (line_count >= kGutterMinLines) {
            // Width of the line-number column. log10-style.
            int w_digits = 1;
            for (int v = line_count; v >= 10; v /= 10) ++w_digits;
            constexpr std::string_view kSep =
                " \xe2\x94\x82 ";  // " │ " (U+2502 = ~3 cells wide visually with pad)
            const std::size_t kSepBytes = kSep.size();
            const Style& gstyle = syntax::gutter();

            // Pre-split runs at newline boundaries so no run spans
            // a line break — block comments in C-style languages emit
            // a single run for the whole comment, including embedded
            // \n. Splitting up-front lets the line-shift remap below
            // be a simple per-line offset add rather than a byte-by-
            // byte run split-walk.
            std::vector<StyledRun> runs_split;
            runs_split.reserve(runs.size() * 2);
            for (const auto& r : runs) {
                std::size_t cur = r.byte_offset;
                const std::size_t end = cur + r.byte_length;
                while (cur < end) {
                    std::size_t nl = out.find('\n', cur);
                    std::size_t seg_end = (nl == std::string::npos || nl >= end)
                                        ? end : nl;
                    if (seg_end > cur) {
                        runs_split.push_back({cur, seg_end - cur, r.style});
                    }
                    if (nl != std::string::npos && nl < end) {
                        runs_split.push_back({nl, 1, r.style});
                        cur = nl + 1;
                    } else {
                        break;
                    }
                }
            }

            // Build line-start offset table for the original `out`.
            std::vector<std::size_t> line_starts;
            line_starts.reserve(static_cast<std::size_t>(line_count) + 1);
            line_starts.push_back(0);
            for (std::size_t k = 0; k < out.size(); ++k) {
                if (out[k] == '\n') line_starts.push_back(k + 1);
            }
            // Sentinel for the binary-search remap below: any run
            // offset is < out.size() < line_starts.back() + ε.

            // Emit out2 with per-line gutter prefixes.
            std::string out2;
            out2.reserve(out.size() + line_starts.size()
                         * (static_cast<std::size_t>(w_digits) + kSepBytes));
            std::vector<StyledRun> runs2;
            runs2.reserve(runs_split.size() + line_starts.size() * 2);

            for (std::size_t i = 0; i < line_starts.size(); ++i) {
                // Gutter for line i+1.
                char buf[24];
                int n = std::snprintf(buf, sizeof(buf), "%*zu",
                                      w_digits, i + 1);
                runs2.push_back({out2.size(), static_cast<std::size_t>(n), gstyle});
                out2.append(buf, static_cast<std::size_t>(n));
                runs2.push_back({out2.size(), kSepBytes, gstyle});
                out2.append(kSep);

                // Line content.
                std::size_t s = line_starts[i];
                std::size_t e = (i + 1 < line_starts.size())
                              ? line_starts[i + 1]
                              : out.size();
                out2.append(out.data() + s, e - s);
            }

            // Remap each split run.
            const std::size_t per_line_shift =
                static_cast<std::size_t>(w_digits) + kSepBytes;
            for (const auto& r : runs_split) {
                // Find which line this run sits on. line_starts is
                // sorted; r.byte_offset is in [line_starts[i],
                // line_starts[i+1]) for the line index i.
                auto it = std::upper_bound(
                    line_starts.begin(), line_starts.end(), r.byte_offset);
                std::size_t line_idx = static_cast<std::size_t>(
                    (it - line_starts.begin())) - 1;
                std::size_t shift = (line_idx + 1) * per_line_shift;
                runs2.push_back({r.byte_offset + shift,
                                 r.byte_length, r.style});
            }

            // Sort by offset for traversal-friendly downstream use.
            std::sort(runs2.begin(), runs2.end(),
                [](const StyledRun& a, const StyledRun& b) {
                    return a.byte_offset < b.byte_offset;
                });

            out = std::move(out2);
            runs = std::move(runs2);
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

// Short-circuit Style merge when the base is default. Style::merge does
// 8 conditional assigns; for an empty `base` the result is just `over`,
// so we can hand it back directly. The check itself is cheap (a few
// loads + an &&-chain that the compiler folds well). Most flatten_inline
// recursion chains feed `inherited` from a prior merge, so `base` is
// rarely empty in steady state — but the helper keeps the common case
// fast and the cold case correct, and it's a single inline that the
// optimiser collapses into the merge body when `base.empty()` would
// otherwise be a constant.
[[nodiscard]] static inline Style fold_style(const Style& base, Style over) noexcept {
    if (base.empty()) return over;
    return base.merge(over);
}

static void flatten_inline(const md::Inline& span, const Style& inherited,
                           std::string& out, std::vector<StyledRun>& runs) {
    std::visit(overload{
        [&](const md::Text& t) {
            runs.push_back({out.size(), t.content.size(), inherited});
            out += t.content;
        },
        [&](const md::Bold& b) {
            auto sty = fold_style(inherited, Style{}.with_bold().with_fg(colors::bold_fg));
            for (auto& child : b.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Italic& it) {
            auto sty = fold_style(inherited, Style{}.with_italic().with_fg(colors::italic_fg));
            for (auto& child : it.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::BoldItalic& bi) {
            auto sty = fold_style(inherited, Style{}.with_bold().with_italic().with_fg(colors::bold_fg));
            for (auto& child : bi.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Code& c) {
            // Inline code: pure colored text, no surrounding space padding.
            // The padding was legacy from a chip-style render that paired
            // with a bg fill; without the bg, the spaces are invisible ink
            // that only widens the word, forcing ugly mid-sentence wraps
            // (e.g. `foo` on its own line, content on the next). Zed / GH
            // CLI style: inline code is just a different color — it reads
            // as distinct without needing a box.
            auto sty = Style{}.with_fg(colors::code_fg);
            runs.push_back({out.size(), c.content.size(), sty});
            out += c.content;
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
            auto sty = fold_style(inherited, Style{}.with_strikethrough().with_fg(colors::strike_fg));
            for (auto& child : s.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Highlight& h) {
            auto sty = fold_style(inherited,
                Style{}.with_bg(colors::highlight_bg).with_fg(colors::highlight_fg));
            for (auto& child : h.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Sub& sb) {
            size_t start = out.size();
            auto sty = fold_style(inherited, Style{}.with_fg(colors::strike_fg));
            out += "_{";
            for (auto& child : sb.children) flatten_inline(child, sty, out, runs);
            out += "}";
            runs.push_back({start, 2, sty});
            runs.push_back({out.size() - 1, 1, sty});
        },
        [&](const md::Sup& sp) {
            size_t start = out.size();
            auto sty = fold_style(inherited, Style{}.with_fg(colors::strike_fg));
            out += "^{";
            for (auto& child : sp.children) flatten_inline(child, sty, out, runs);
            out += "}";
            runs.push_back({start, 2, sty});
            runs.push_back({out.size() - 1, 1, sty});
        },
        [&](const md::Kbd& k) {
            auto bracket_sty = Style{}.with_fg(colors::kbd_border);
            auto inner_sty = fold_style(inherited,
                Style{}.with_bold().with_fg(colors::kbd_fg));
            runs.push_back({out.size(), 1, bracket_sty});
            out += "[";
            for (auto& child : k.children) flatten_inline(child, inner_sty, out, runs);
            runs.push_back({out.size(), 1, bracket_sty});
            out += "]";
        },
        [&](const md::Abbr& a) {
            auto sty = fold_style(inherited, Style{}.with_underline());
            for (auto& child : a.children) flatten_inline(child, sty, out, runs);
            if (!a.title.empty()) {
                std::string suffix = " (" + a.title + ")";
                auto suf_sty = Style{}.with_fg(colors::footnote_fg).with_italic();
                runs.push_back({out.size(), suffix.size(), suf_sty});
                out += suffix;
            }
        },
        [&](const md::Mention& m) {
            auto sty = Style{}.with_fg(colors::mention_fg);
            runs.push_back({out.size(), m.display.size(), sty});
            out += m.display;
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

        // Hanging-indent layout: render the bullet/number marker as its own
        // fixed-width column on the left, and the body content in a flexing
        // column on the right. When the body wraps, continuation lines stay
        // aligned under the first character of the body — they do not bleed
        // back to column 0. Same hstack(prefix, body) pattern blockquote uses.
        auto prefix_elem = Element{TextElement{
            .content = prefix,
            .style = prefix_style,
        }};

        std::string body;
        std::vector<StyledRun> body_runs;
        Style base = Style{}.with_fg(colors::text);
        for (auto& s : item.spans) {
            flatten_inline(s, base, body, body_runs);
        }
        Element body_elem = (body_runs.size() == 1)
            ? Element{TextElement{
                .content = std::move(body),
                .style   = body_runs[0].style,
              }}
            : Element{TextElement{
                .content = std::move(body),
                .style   = base,
                .runs    = std::move(body_runs),
              }};

        auto item_row = detail::hstack()(std::move(prefix_elem),
                                         std::move(body_elem));

        if (item.children.empty()) {
            items.push_back(std::move(item_row));
        } else {
            // Item with sub-content (nested lists, multi-para)
            std::vector<Element> sub;
            sub.reserve(item.children.size() + 1);
            sub.push_back(std::move(item_row));
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

            std::string content;
            std::vector<StyledRun> runs;
            for (auto& s : h.spans) {
                flatten_inline(s, sty, content, runs);
            }
            Element heading_text = Element{TextElement{
                .content = std::move(content),
                .style = sty,
                .runs = std::move(runs),
            }};

            // Underline rule for # / ## only — gives the heading
            // typographic weight without compromising monotonicity:
            // headings commit atomically, so the +1 row arrives in
            // a single snap and never reflows. h3+ stay
            // single-line because they're already visually distinct
            // via the bold + heading_dim treatment, and adding a
            // rule under every h3 in a tutorial-style doc would be
            // visual noise.
            //
            // Glyph choice: U+2550 (═) for h1 — heaviest, matches
            // the "section break" feel; U+2500 (─) for h2 —
            // lighter, matches the "subsection" feel. Both rendered
            // in heading_rule (= bright_black) so they sit visually
            // *under* the heading text without competing with it.
            if (h.level == 1 || h.level == 2) {
                const char* rule_glyph =
                    (h.level == 1) ? "\xe2\x95\x90"   // ═ U+2550
                                   : "\xe2\x94\x80"; // ─ U+2500
                const Style rule_style =
                    Style{}.with_fg(colors::heading_rule).with_dim();
                Element rule = Element{ComponentElement{
                    .render = [rule_glyph, rule_style]
                              (int w, int /*h*/) -> Element {
                        if (w <= 0) return Element{TextElement{}};
                        std::string line;
                        line.reserve(static_cast<std::size_t>(w) * 3);
                        for (int i = 0; i < w; ++i) line.append(rule_glyph);
                        return Element{TextElement{
                            .content = std::move(line),
                            .style   = rule_style,
                        }};
                    },
                }};
                return detail::vstack()(
                    std::move(heading_text), std::move(rule)
                ).build();
            }
            return heading_text;
        },
        [](const md::CodeBlock& c) -> Element {
            // Zed style: round border, subtle bg, language label top-left.
            // align_self(Stretch) anchors the right border at the parent's
            // available width instead of the code's natural width.
            // Without it, the border tracks the longest line — which
            // changes during streaming as new lines arrive — so the
            // right border drifts column-to-column between frames
            // (visible later as phantom borders left in scrollback) and
            // the panel reads as "not responsive" once the message
            // settles.
            auto builder = detail::vstack()
                .align_self(Align::Stretch)
                .border(BorderStyle::Round)
                .border_color(colors::code_border)
                .padding(0, 1, 0, 1);

            if (!c.lang.empty()) {
                std::string label = " " + c.lang + " ";
                // Diagram/math fences we can't layout: flag them in the label
                // so users know the content is a textual fallback, not the
                // intended rendering.
                if (c.lang == "mermaid" || c.lang == "matrix" ||
                    c.lang == "math"    || c.lang == "latex"  ||
                    c.lang == "dot"     || c.lang == "graphviz") {
                    label = " " + c.lang + " (diagram) ";
                }
                builder = std::move(builder).border_text(
                    std::move(label),
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

            // ── Pre-flatten every cell's inline content into (content, runs).
            // This is the parse-time work: the inline spans are converted
            // once into a flat string + StyledRun list which the render-
            // time wrapper can then slice into wrapped lines based on the
            // ACTUAL available width. Keeps the runtime path cheap (just
            // wrap + pad) instead of re-walking the inline AST every frame.
            struct FlatCell { std::string content; std::vector<StyledRun> runs; };

            std::vector<FlatCell> header_flat;
            header_flat.reserve(static_cast<size_t>(ncols));
            auto header_base = Style{}.with_bold().with_fg(colors::table_header);
            for (int c = 0; c < ncols; ++c) {
                FlatCell f;
                for (auto& s : tbl.header.cells[static_cast<size_t>(c)].spans)
                    flatten_inline(s, header_base, f.content, f.runs);
                header_flat.push_back(std::move(f));
            }

            std::vector<std::vector<FlatCell>> rows_flat;
            rows_flat.reserve(tbl.rows.size());
            auto cell_base = Style{}.with_fg(colors::text);
            for (auto& row : tbl.rows) {
                std::vector<FlatCell> rf;
                rf.reserve(static_cast<size_t>(ncols));
                for (int c = 0; c < ncols; ++c) {
                    FlatCell f;
                    if (static_cast<size_t>(c) < row.cells.size()) {
                        for (auto& s : row.cells[static_cast<size_t>(c)].spans)
                            flatten_inline(s, cell_base, f.content, f.runs);
                    }
                    rf.push_back(std::move(f));
                }
                rows_flat.push_back(std::move(rf));
            }

            // ── Ideal width per column (longest cell content). Used to
            // decide the layout when the terminal is wide enough; shrunk
            // proportionally when not. No cap — we let the render-time
            // distributor handle it.
            std::vector<int> ideal(static_cast<size_t>(ncols), 0);
            for (int c = 0; c < ncols; ++c) {
                ideal[static_cast<size_t>(c)] =
                    std::max(1, string_width(header_flat[static_cast<size_t>(c)].content));
                for (auto& r : rows_flat) {
                    ideal[static_cast<size_t>(c)] = std::max(
                        ideal[static_cast<size_t>(c)],
                        string_width(r[static_cast<size_t>(c)].content));
                }
            }

            // ── Shared helpers for layout + render ───────────────────────
            // Both `measure` (called during build_layout_tree) and
            // `render` (called during paint) need to know how the table
            // lays out at a given width. The `measure` path needs only
            // the row count so the layout engine allocates the right
            // amount of vertical space; the `render` path needs the full
            // wrapped Elements. Both call distribute_cols + wrap_cell —
            // identical math, so the row count from measure exactly
            // matches what render produces.
            //
            // The table data lives in a shared_ptr so both the `measure`
            // and `render` closures can capture it by value (without
            // double-moving). Capturing by reference would leave both
            // callbacks holding dangling refs to function-local state
            // — segfault on the first paint after this function returns.

            auto distribute_cols = [](int avail_w, int ncols_,
                                       const std::vector<int>& ideal_)
                                       -> std::vector<int> {
                constexpr int pad = 1;
                int chrome = 1 + ncols_ + 2 * ncols_ * pad;
                int avail_cells = std::max(ncols_, avail_w - chrome);
                constexpr int kMinCol = 6;
                int ideal_sum = 0;
                for (int v : ideal_) ideal_sum += v;
                std::vector<int> col_w(static_cast<size_t>(ncols_));
                if (ideal_sum <= avail_cells) {
                    for (int c = 0; c < ncols_; ++c)
                        col_w[static_cast<size_t>(c)] = ideal_[static_cast<size_t>(c)];
                } else {
                    int min_total = kMinCol * ncols_;
                    if (avail_cells <= min_total) {
                        for (int c = 0; c < ncols_; ++c)
                            col_w[static_cast<size_t>(c)] = kMinCol;
                    } else {
                        int budget_above_min = avail_cells - min_total;
                        int excess_total = 0;
                        for (int v : ideal_) excess_total += std::max(0, v - kMinCol);
                        for (int c = 0; c < ncols_; ++c) {
                            int excess = std::max(0,
                                ideal_[static_cast<size_t>(c)] - kMinCol);
                            int share = excess_total > 0
                                ? (excess * budget_above_min) / excess_total
                                : budget_above_min / ncols_;
                            col_w[static_cast<size_t>(c)] = kMinCol + share;
                        }
                        int sum = 0;
                        for (int v : col_w) sum += v;
                        int rem = avail_cells - sum;
                        for (int i = 0; i < rem; ++i) {
                            int best = 0;
                            for (int c = 1; c < ncols_; ++c)
                                if (ideal_[static_cast<size_t>(c)] - col_w[static_cast<size_t>(c)]
                                    > ideal_[static_cast<size_t>(best)] - col_w[static_cast<size_t>(best)])
                                    best = c;
                            ++col_w[static_cast<size_t>(best)];
                        }
                    }
                }
                return col_w;
            };

            // Word-aware wrap with force-break for over-long words. Used
            // by both the row-counter (measure path) and the cell
            // renderer (paint path); identical to keep counts exact.
            auto wrap_cell_lines = [](const std::string& content, int max_w) -> int {
                if (max_w <= 0) max_w = 1;
                if (content.empty()) return 1;
                auto cb = [](unsigned char c) -> int {
                    if (c < 0xC0) return 1;
                    if (c < 0xE0) return 2;
                    if (c < 0xF0) return 3;
                    return 4;
                };
                int lines = 0;
                size_t i = 0;
                int line_w = 0;
                bool any_in_line = false;
                auto end_line = [&]() { ++lines; line_w = 0; any_in_line = false; };
                while (i < content.size()) {
                    if (content[i] == '\n') { end_line(); ++i; continue; }
                    size_t tok_start = i;
                    bool ws = (content[i] == ' ' || content[i] == '\t');
                    int tok_w = 0;
                    while (i < content.size() && content[i] != '\n'
                           && ((content[i] == ' '
                                || content[i] == '\t') == ws)) {
                        int n = cb(static_cast<unsigned char>(content[i]));
                        ++tok_w;
                        i += static_cast<size_t>(n);
                    }
                    if (line_w + tok_w <= max_w) {
                        line_w += tok_w;
                        if (!ws) any_in_line = true;
                        continue;
                    }
                    if (ws) { end_line(); continue; }
                    if (any_in_line) { end_line(); i = tok_start; continue; }
                    size_t pos = tok_start;
                    int forced = 0;
                    while (pos < i && forced < max_w) {
                        int n = cb(static_cast<unsigned char>(content[pos]));
                        pos += static_cast<size_t>(n);
                        ++forced;
                    }
                    end_line();
                    i = pos;
                }
                if (any_in_line) ++lines;
                return std::max(1, lines);
            };

            // Bundle the per-table state into a heap-allocated struct so
            // both closures (render + measure) can hold it by shared_ptr
            // and survive past this function's return.
            struct TableData {
                int ncols;
                std::vector<FlatCell> header_flat;
                std::vector<std::vector<FlatCell>> rows_flat;
                std::vector<int> ideal;
                Style header_base;
                Style cell_base;
                // Width-keyed render cache. Cell content is fixed at
                // parse time; only the wrapping (and therefore the row
                // count) depends on the available width. The closure
                // below is otherwise the most expensive
                // md_block_to_element output — every paint re-wraps
                // every cell and re-builds every line. Memoising on
                // `avail_w` collapses the steady-state cost to a
                // single Element copy: the renderer's component_cache
                // already short-circuits render() entirely on a hit
                // there, so this kicks in on the wider class of cache
                // misses (component_cache eviction, fresh
                // ComponentElement instance after a prefix
                // regeneration that re-materialised this same table
                // block at a new tree slot, …).
                //
                // mutable: the render lambda runs on a const-ref to
                // TableData via the shared_ptr; the cache slot is
                // logically const (same input always yields same
                // output) so the standard memoisation justification
                // for `mutable` applies.
                mutable int     cached_w = -1;
                mutable Element cached_render;
            };
            auto data = std::make_shared<TableData>(TableData{
                ncols,
                std::move(header_flat),
                std::move(rows_flat),
                std::move(ideal),
                header_base,
                cell_base,
            });

            // Wrap into a ComponentElement so cell wrapping uses the
            // actual rendered width. The `measure` callback reports the
            // exact row count so the parent vstack allocates the right
            // amount of vertical space — without it the layout engine
            // defaults to {max_width, 1}, clipping the entire table to
            // a single row (the original symptom).
            return Element{ComponentElement{
                .render = [data, distribute_cols]
                          (int avail_w, int /*h*/) -> Element {
                    // Width-cache fast path. Returns a copy of the
                    // cached Element — cheap relative to re-running
                    // the wrap pipeline, and unavoidable because the
                    // signature returns by value. The component_cache
                    // ABOVE this catches the more common
                    // (instance, width) hit and skips render()
                    // entirely; this layer is the one that survives
                    // component_cache evictions and prefix-regen
                    // rebuilds (where the table's outer
                    // ComponentElement gets a new address but `data`
                    // — captured via shared_ptr — is preserved).
                    if (avail_w == data->cached_w) return data->cached_render;

                    int ncols = data->ncols;
                    const auto& header_flat = data->header_flat;
                    const auto& rows_flat   = data->rows_flat;
                    const auto& header_base = data->header_base;
                    const auto& cell_base   = data->cell_base;
                    constexpr int pad = 1;
                    auto col_w = distribute_cols(avail_w, ncols, data->ideal);

                    // ── Wrap a (content, runs) cell to a target width.
                    // Returns one entry per visual line; each carries its
                    // own sliced run list so per-character styling
                    // (bold, code, color) survives the wrap intact.
                    // Word-aware with force-break for words longer than
                    // the target width. Hard '\n' in content also breaks.
                    auto wrap_cell = [](const std::string& content,
                                        const std::vector<StyledRun>& runs,
                                        int max_w) -> std::vector<FlatCell> {
                        std::vector<FlatCell> out;
                        if (max_w <= 0) max_w = 1;
                        if (content.empty()) {
                            out.push_back({});
                            return out;
                        }

                        // Helper: bytes per UTF-8 char from leading byte.
                        auto cb = [](unsigned char c) -> int {
                            if (c < 0x80) return 1;
                            if (c < 0xC0) return 1;     // invalid lead, treat as 1
                            if (c < 0xE0) return 2;
                            if (c < 0xF0) return 3;
                            return 4;
                        };

                        // Find ranges [start, end) per output line. Then
                        // build content + runs from those ranges.
                        std::vector<std::pair<size_t, size_t>> lines;
                        size_t i = 0;
                        size_t line_start = 0;
                        int line_w = 0;

                        auto flush_line = [&](size_t end) {
                            // Trim trailing whitespace from the line.
                            size_t e = end;
                            while (e > line_start
                                   && (content[e - 1] == ' '
                                       || content[e - 1] == '\t'))
                                --e;
                            lines.push_back({line_start, e});
                        };

                        while (i < content.size()) {
                            // Hard newline → flush current line, start fresh.
                            if (content[i] == '\n') {
                                flush_line(i);
                                ++i;
                                line_start = i;
                                line_w = 0;
                                continue;
                            }
                            // Read next token (run of non-space OR run of
                            // space). Spaces collapse only across line
                            // breaks, not within a line.
                            size_t tok_start = i;
                            bool ws = (content[i] == ' ' || content[i] == '\t');
                            int tok_w = 0;
                            while (i < content.size()
                                   && content[i] != '\n'
                                   && ((content[i] == ' '
                                        || content[i] == '\t') == ws)) {
                                int n = cb(static_cast<unsigned char>(content[i]));
                                ++tok_w;
                                i += static_cast<size_t>(n);
                            }

                            if (line_w + tok_w <= max_w) {
                                line_w += tok_w;
                                continue;
                            }
                            // Doesn't fit. If the token is whitespace,
                            // discard it at the line break.
                            if (ws) {
                                flush_line(tok_start);
                                line_start = i;
                                line_w = 0;
                                continue;
                            }
                            // Non-whitespace word that doesn't fit.
                            // Case A: line already has content → break here.
                            if (line_w > 0) {
                                flush_line(tok_start);
                                line_start = tok_start;
                                line_w = 0;
                                i = tok_start;   // re-process the word on new line
                                continue;
                            }
                            // Case B: a single word longer than the line.
                            // Force-break at max_w characters.
                            size_t pos = tok_start;
                            int forced_w = 0;
                            while (pos < i && forced_w < max_w) {
                                int n = cb(static_cast<unsigned char>(content[pos]));
                                pos += static_cast<size_t>(n);
                                ++forced_w;
                            }
                            flush_line(pos);
                            line_start = pos;
                            line_w = 0;
                            i = pos;             // continue word on next line
                        }
                        if (line_start < content.size() || lines.empty())
                            flush_line(content.size());

                        // Build output FlatCell per line range, slicing runs.
                        out.reserve(lines.size());
                        for (auto [a, b] : lines) {
                            FlatCell fc;
                            fc.content = content.substr(a, b - a);
                            for (auto& r : runs) {
                                size_t rs = r.byte_offset;
                                size_t re = r.byte_offset + r.byte_length;
                                if (re <= a || rs >= b) continue;
                                size_t s = std::max(rs, a);
                                size_t e = std::min(re, b);
                                if (e <= s) continue;
                                fc.runs.push_back(
                                    StyledRun{s - a, e - s, r.style});
                            }
                            out.push_back(std::move(fc));
                        }
                        return out;
                    };

                    // ── Build the visual rows for a logical row by wrapping
                    // each cell to its column width and then transposing
                    // — visual line v of the row is "cell[0].line[v] │
                    // cell[1].line[v] │ …", padded with empty lines
                    // when a cell is shorter than the row's tallest cell.
                    auto build_row_visuals = [&](
                        const std::vector<FlatCell>& cells,
                        const Style& base) -> std::vector<Element>
                    {
                        // Wrap each cell.
                        std::vector<std::vector<FlatCell>> wrapped(
                            static_cast<size_t>(ncols));
                        int max_lines = 1;
                        for (int c = 0; c < ncols; ++c) {
                            wrapped[static_cast<size_t>(c)] = wrap_cell(
                                cells[static_cast<size_t>(c)].content,
                                cells[static_cast<size_t>(c)].runs,
                                col_w[static_cast<size_t>(c)]);
                            max_lines = std::max(max_lines,
                                static_cast<int>(wrapped[static_cast<size_t>(c)].size()));
                        }
                        auto sep_style = Style{}.with_fg(colors::table_border);
                        const std::string sep = "\xe2\x94\x82";   // │

                        std::vector<Element> visuals;
                        visuals.reserve(static_cast<size_t>(max_lines));
                        for (int v = 0; v < max_lines; ++v) {
                            std::string line;
                            std::vector<StyledRun> line_runs;
                            // Leading │
                            line += sep;
                            line_runs.push_back(StyledRun{0, sep.size(), sep_style});
                            for (int c = 0; c < ncols; ++c) {
                                int cw = col_w[static_cast<size_t>(c)];
                                int total = cw + pad * 2;
                                // Pull this cell's v-th line if present.
                                const FlatCell* fc = nullptr;
                                if (static_cast<int>(wrapped[static_cast<size_t>(c)].size()) > v)
                                    fc = &wrapped[static_cast<size_t>(c)][static_cast<size_t>(v)];

                                std::string content_part;
                                std::vector<StyledRun> runs_part;
                                if (fc) {
                                    content_part = fc->content;
                                    runs_part = fc->runs;
                                }
                                int content_w = string_width(content_part);

                                // Left pad
                                size_t cell_off = line.size();
                                line.append(static_cast<size_t>(pad), ' ');
                                // Content
                                size_t content_off = line.size();
                                line += content_part;
                                for (auto& r : runs_part)
                                    line_runs.push_back(StyledRun{
                                        content_off + r.byte_offset,
                                        r.byte_length, r.style});
                                // Right pad to fill the column
                                int right = std::max(0, cw - content_w) + pad;
                                line.append(static_cast<size_t>(right), ' ');
                                // Cell-spanning base style for the
                                // padding cells (so background-color
                                // styles, if any are added later, fill
                                // the whole cell instead of just text).
                                if (!runs_part.empty() || content_w == 0) {
                                    line_runs.push_back(StyledRun{
                                        cell_off, static_cast<size_t>(pad), base});
                                    line_runs.push_back(StyledRun{
                                        content_off + content_part.size(),
                                        static_cast<size_t>(right), base});
                                }
                                // Trailing │
                                size_t s = line.size();
                                line += sep;
                                line_runs.push_back(StyledRun{s, sep.size(), sep_style});
                                (void)total;
                            }
                            visuals.push_back(Element{TextElement{
                                .content = std::move(line),
                                .style = base,
                                .runs = std::move(line_runs),
                            }});
                        }
                        return visuals;
                    };

                    // ── Border line builder. type: 0=top, 1=mid, 2=bottom.
                    auto make_border_line = [&](int type) -> Element {
                        const char* left[]  = {"\xe2\x94\x8c","\xe2\x94\x9c","\xe2\x94\x94"};
                        const char* mid[]   = {"\xe2\x94\xac","\xe2\x94\xbc","\xe2\x94\xb4"};
                        const char* right[] = {"\xe2\x94\x90","\xe2\x94\xa4","\xe2\x94\x98"};
                        std::string line;
                        line += left[type];
                        for (int c = 0; c < ncols; ++c) {
                            if (c > 0) line += mid[type];
                            int total = col_w[static_cast<size_t>(c)] + pad * 2;
                            for (int k = 0; k < total; ++k)
                                line += "\xe2\x94\x80";          // ─
                        }
                        line += right[type];
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = Style{}.with_fg(colors::table_border),
                        }};
                    };

                    // ── Soft inter-row separator. Same skeleton as the
                    // header rule (├ … ┼ … ┤) but with light-quadruple-
                    // dash horizontals (┈) and rendered at half luminance
                    // via `with_dim`. Dashed-not-solid + dim is the
                    // canonical "this is a row break, not a section
                    // break" treatment in pro TUIs (Helix, Lazygit).
                    auto make_row_separator = [&]() -> Element {
                        std::string line;
                        line += "\xe2\x94\x9c";          // ├
                        for (int c = 0; c < ncols; ++c) {
                            if (c > 0) line += "\xe2\x94\xbc";  // ┼
                            int total = col_w[static_cast<size_t>(c)] + pad * 2;
                            for (int k = 0; k < total; ++k)
                                line += "\xe2\x94\x88";  // ┈ (light quadruple dash)
                        }
                        line += "\xe2\x94\xa4";          // ┤
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = Style{}.with_fg(colors::table_border).with_dim(),
                        }};
                    };

                    // ── Compose the full table.
                    std::vector<Element> rows;
                    rows.reserve(rows_flat.size() * 3 + 4);
                    rows.push_back(make_border_line(0));
                    for (auto& v : build_row_visuals(header_flat, header_base))
                        rows.push_back(std::move(v));
                    rows.push_back(make_border_line(1));
                    for (std::size_t ri = 0; ri < rows_flat.size(); ++ri) {
                        if (ri > 0) rows.push_back(make_row_separator());
                        for (auto& v : build_row_visuals(rows_flat[ri], cell_base))
                            rows.push_back(std::move(v));
                    }
                    rows.push_back(make_border_line(2));
                    auto built = detail::vstack()(std::move(rows)).build();
                    // Memoise. Subsequent paints at the same width
                    // hand back the cached copy without re-walking
                    // any cell. Different widths overwrite the slot
                    // (a single-entry cache is enough — terminal
                    // resizes are rare relative to per-frame paints,
                    // and we only ever render at one width per pass).
                    data->cached_w      = avail_w;
                    data->cached_render = built;
                    return built;
                },
                // No custom measure — the renderer auto-derives the
                // table's height by running the render callback and
                // counting the rows of the produced tree.  This
                // removes the previous hand-rolled measure that had to
                // mirror render() exactly (and caused clipped rows
                // when the two formulas drifted).
                .layout = {},
            }};
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
        [](const md::Alert& a) -> Element {
            // GitHub-style alert: colored gutter + kind header, children
            // indented under a single solid bar in the kind color.
            const char* label = "NOTE";
            const char* icon  = "\xe2\x84\xb9";  // ℹ
            Color bar = colors::alert_note;
            switch (a.kind) {
                case md::Alert::Kind::Note:
                    label = "NOTE";  icon = "\xe2\x84\xb9";  // ℹ
                    bar = colors::alert_note; break;
                case md::Alert::Kind::Tip:
                    label = "TIP";   icon = "\xf0\x9f\x92\xa1"; // 💡
                    bar = colors::alert_tip; break;
                case md::Alert::Kind::Important:
                    label = "IMPORTANT"; icon = "\xe2\x97\x86";  // ◆
                    bar = colors::alert_important; break;
                case md::Alert::Kind::Warning:
                    label = "WARNING";   icon = "\xe2\x9a\xa0";  // ⚠
                    bar = colors::alert_warning; break;
                case md::Alert::Kind::Caution:
                    label = "CAUTION";   icon = "\xe2\x9b\x94";  // ⛔
                    bar = colors::alert_caution; break;
            }

            auto bar_style = Style{}.with_fg(bar).with_bold();

            std::vector<Element> rows;
            rows.reserve(a.children.size() + 1);

            // Header: " ▍ ICON KIND "
            std::string header;
            header.reserve(16);
            header += "\xe2\x96\x8d ";          // ▍
            header += icon;
            header += ' ';
            header += label;
            rows.push_back(Element{TextElement{
                .content = std::move(header),
                .style = bar_style,
            }});

            for (auto& child : a.children) {
                auto child_elem = md_block_to_element(child);
                rows.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = "\xe2\x96\x8d ",     // ▍
                        .style = bar_style,
                    }},
                    std::move(child_elem)
                ));
            }

            return detail::vstack()(std::move(rows));
        },
        [](const md::DefList& d) -> Element {
            std::vector<Element> items;
            items.reserve(d.items.size() * 2);
            for (auto& item : d.items) {
                // Term: bolded
                std::string term_text;
                std::vector<StyledRun> runs;
                Style base = Style{}.with_bold().with_fg(colors::bold_fg);
                for (auto& s : item.term) flatten_inline(s, base, term_text, runs);
                items.push_back(Element{TextElement{
                    .content = std::move(term_text),
                    .style = base,
                    .runs = std::move(runs),
                }});

                // Definitions: indented with leading ":"
                for (auto& def : item.defs) {
                    std::vector<Element> def_rows;
                    def_rows.reserve(def.size());
                    for (auto& child : def) {
                        def_rows.push_back(md_block_to_element(child));
                    }
                    auto def_body = detail::vstack()(std::move(def_rows));
                    items.push_back(detail::hstack()(
                        Element{TextElement{
                            .content = "  : ",
                            .style = Style{}.with_fg(colors::list_bullet),
                        }},
                        std::move(def_body)
                    ));
                }
            }
            return detail::vstack()(std::move(items));
        },
        [](const md::Details& d) -> Element {
            // Render as: "▸ summary" bolded, body indented beneath.
            std::string summary_text;
            std::vector<StyledRun> runs;
            Style base = Style{}.with_bold().with_fg(colors::bold_fg);
            runs.push_back({0, 2, Style{}.with_fg(colors::list_bullet)});
            summary_text += "\xe2\x96\xb8 ";  // ▸
            size_t offset = summary_text.size();
            for (auto& s : d.summary) flatten_inline(s, base, summary_text, runs);
            for (size_t i = 1; i < runs.size(); ++i) {
                // Offsets from flatten_inline are relative to `out` which
                // already includes the "▸ " prefix, so nothing to shift.
                (void)offset;
            }
            Element header{TextElement{
                .content = std::move(summary_text),
                .style = base,
                .runs = std::move(runs),
            }};

            std::vector<Element> body_rows;
            body_rows.reserve(d.body.size());
            for (auto& b : d.body) body_rows.push_back(md_block_to_element(b));

            std::vector<Element> out_rows;
            out_rows.reserve(2);
            out_rows.push_back(std::move(header));
            if (!body_rows.empty()) {
                out_rows.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = "  ",
                    }},
                    detail::vstack()(std::move(body_rows))
                ));
            }
            return detail::vstack()(std::move(out_rows));
        },
        [](const md::HtmlBlock& h) -> Element {
            // Raw-HTML passthrough: render as subtle preformatted text.
            return Element{TextElement{
                .content = h.content,
                .style = Style{}.with_fg(colors::footnote_fg),
            }};
        },
    }, block.inner);
}

// Build the Element tree from a freshly-parsed document. Pulled out of
// `markdown()` so the memo path below can share the assembly logic
// without duplicating the empty/one-block/many-block branches.
namespace {

[[nodiscard]] Element assemble_markdown(md::Document&& doc) {
    if (doc.blocks.empty()) return Element{TextElement{""}};

    if (doc.blocks.size() == 1)
        return detail::vstack().padding(0, 0, 0, 2)(
            md_block_to_element(doc.blocks[0]));

    std::vector<Element> blocks;
    blocks.reserve(doc.blocks.size());
    for (auto& block : doc.blocks)
        blocks.push_back(md_block_to_element(block));

    return detail::vstack().gap(1).padding(0, 0, 0, 2)(std::move(blocks));
}

// ── markdown() content-keyed LRU memo ───────────────────────────────────────
//
// Without this, every `maya::Turn` that uses `Turn::MarkdownText{...}`
// (the common path for settled assistant messages) re-runs
// parse_markdown + the full block-to-Element walk on every frame the
// renderer requests it. In a moha session with N settled turns and one
// live composer, that's O(sum of body bytes) per keystroke — the
// dominant per-frame cost the user reports as "lag grows with
// conversation length."
//
// Strategy:
//   * Bound the cache to a small fixed entry count (kCap). Each entry
//     owns a copy of the source string and a shared_ptr<const Element>
//     to the built tree. Memory ceiling is ~kCap × avg_body_size ≈
//     low-MB for typical moha sessions (kCap=64, ~32KB/turn).
//   * Key on a 64-bit content hash plus exact length match plus a
//     memcmp tiebreak. The combined check is collision-proof; the
//     hash alone narrows the search to ~1 candidate.
//   * MRU promotion via swap-to-back so LRU eviction is `pop_back`.
//   * shared_ptr<const Element>: caller receives the cached tree via
//     the Element(shared_ptr<const Element>) implicit conversion,
//     which auto-derives a stable cache_id from the control block.
//     The renderer's content-keyed component_cache then short-circuits
//     layout AND paint of the cached subtree to a cells-blit — so the
//     win compounds: parse skipped here, tree walk skipped in the
//     renderer.
//
// All state is thread_local — the renderer runs on a single thread per
// app, so no synchronisation is needed and the cache stays warm across
// the whole UI thread's lifetime.

constexpr std::size_t kMarkdownCacheCap = 64;

// FNV-1a 64-bit over the source bytes. ~1 cycle/byte on the most
// constrained ARMv6 target; for a 10 KB body that's well under 50 µs
// on a Raspberry Pi 1 — and it's only paid on cache miss anyway since
// the hot path takes the hit branch below.
[[nodiscard]] inline std::uint64_t hash_markdown_source(
    std::string_view s) noexcept
{
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x100000001b3ULL;
    std::uint64_t h = kOffset;
    for (unsigned char c : s) {
        h ^= c;
        h *= kPrime;
    }
    return h;
}

struct MarkdownCacheEntry {
    std::uint64_t hash;
    std::string   source;       // owned for memcmp tiebreak
    Element       built;        // assembled Element tree; copied on hit
};

[[nodiscard]] inline std::vector<MarkdownCacheEntry>& markdown_cache() {
    thread_local std::vector<MarkdownCacheEntry> cache;
    return cache;
}

} // anonymous

Element markdown(std::string_view source) {
    // ── Tiny inputs: skip the cache entirely.
    // The memo overhead (hash + linear probe + LRU bookkeeping) is
    // larger than parsing a few bytes of markdown. Threshold is
    // deliberately small — most "trivially short" body slots in moha
    // (single-token labels, "(empty)" placeholders) take this path
    // and don't push real settled content out of the cache.
    if (source.size() < 32) {
        return assemble_markdown(parse_markdown(source));
    }

    auto& cache = markdown_cache();
    const std::uint64_t h = hash_markdown_source(source);

    // ── Hit path: linear scan (cache is small, predictable, branch-friendly).
    // On hit we MRU-promote by swapping the entry to the tail and
    // return a deep copy of the cached Element. NOTE: we intentionally
    // do NOT wrap the result in a shared_ptr (which would auto-cast
    // into a ComponentElement via the implicit Element ctor). Wrapping
    // would let the renderer's content-keyed component_cache cells-blit
    // future paints, BUT it forces the auto-measure path to run an
    // extra inner layout::compute on every cache miss — exactly the
    // regression that broke test_markdown's deep_blockquote stress
    // case. Apps that want cells-cache acceleration should opt in at
    // the WIDGET level (Turn::cache_id) where the outer wrapper makes
    // the inner markdown free anyway. The parse-only memo here is
    // strictly additive — it saves the bytes-to-AST work for
    // unchanged content, nothing else.
    for (std::size_t i = 0; i < cache.size(); ++i) {
        auto& e = cache[i];
        if (e.hash == h
            && e.source.size() == source.size()
            && std::memcmp(e.source.data(), source.data(), source.size()) == 0)
        {
            if (i + 1 != cache.size()) std::swap(cache[i], cache.back());
            return cache.back().built;   // variant copy, O(tree)
        }
    }

    // ── Miss: parse, assemble, store, evict oldest if needed.
    Element built = assemble_markdown(parse_markdown(source));
    if (cache.size() >= kMarkdownCacheCap) {
        // Drop the LRU (front) — single shift only happens when the
        // cache is full, which is itself the steady state, but the
        // shift cost is O(kCap × sizeof(entry)) ≈ small for kCap=64,
        // and dwarfed by the parse work we just did on this miss.
        cache.erase(cache.begin());
    }
    cache.push_back({h, std::string{source}, built});
    return built;
}

// ============================================================================
// StreamingMarkdown — progressive per-block rendering
// ============================================================================

size_t StreamingMarkdown::find_block_boundary() noexcept {
    // Resumable scan — see the scan_* / in_code_fence_ comments in the
    // header for the full design. The body below is byte-for-byte the
    // same boundary detection logic the prior const version had; only
    // the bookkeeping around it (cursor / fence parity / last-boundary)
    // is now persisted across calls instead of re-derived from
    // committed_ every time.
    //
    // The scan walks ONE LINE START at a time: i points at the byte
    // following a '\n' (or i == 0).  Detecting the blank-line separator
    // looks BACKWARD at line start: source_[i] == '\n' implies
    // source_[i-1] was also '\n' (or i == 0), i.e. we just crossed a
    // \n\n separator. Same correction applies to the heading marker.
    size_t i             = scan_cursor_;
    bool   in_fence      = scan_in_fence_;
    size_t last_boundary = scan_last_boundary_;

    while (i < source_.size()) {
        bool at_line_start = (i == 0 || source_[i - 1] == '\n');

        if (at_line_start) {
            bool is_code_fence = i + 3 <= source_.size() &&
                ((source_[i] == '`' && source_[i+1] == '`' && source_[i+2] == '`') ||
                 (source_[i] == '~' && source_[i+1] == '~' && source_[i+2] == '~'));
            bool is_math_fence = !is_code_fence && i + 2 <= source_.size() &&
                source_[i] == '$' && source_[i+1] == '$';
            if (is_code_fence || is_math_fence) {
                // Opening fence commits any prose that preceded it: the
                // paragraph above can be rendered immediately with its
                // final styling, even if no blank line separates them.
                if (!in_fence) last_boundary = i;
                size_t eol = source_.find('\n', i);
                if (eol == std::string::npos) break;
                in_fence = !in_fence;
                i = eol + 1;
                if (!in_fence) last_boundary = i;
                continue;
            }

            if (!in_fence) {
                // Blank line: we're at the second '\n' of a \n\n pair
                // (or source begins with '\n' at i == 0).  Commit up to
                // and including the second '\n'; the next block starts
                // at i + 1.
                if (source_[i] == '\n') {
                    last_boundary = i + 1;
                    i = i + 1;
                    continue;
                }
                // ATX heading at line start.  Commit the prose before
                // the heading; the heading line itself stays in the
                // tail until a later boundary (next blank, next heading,
                // fence open, or stream end) advances past it.  A list
                // marker (`-` `*` `+`) / blockquote (`>`) / table row
                // (`|`) is deliberately NOT treated as a boundary — a
                // list/blockquote/table is a single cohesive block and
                // committing each row separately would stretch a tight
                // list into singletons separated by the inter-block gap.
                if (source_[i] == '#') {
                    last_boundary = i;
                }
            }
        }

        size_t eol = source_.find('\n', i);
        if (eol == std::string::npos) break;
        i = eol + 1;
    }

    // Persist where the scanner stopped so the next call resumes here.
    // i is either == source_.size() (drained), or pointing at a byte
    // we couldn't advance past (find('\n', i) returned npos because
    // the current line hasn't terminated yet) — both correct resume
    // points; new bytes appended to source_ will be visible on the
    // next call without re-walking the prefix.
    scan_cursor_        = i;
    scan_in_fence_      = in_fence;
    scan_last_boundary_ = last_boundary;
    return last_boundary;
}

// Parse [committed_, boundary), extend ref_defs_ with any new defs, push
// rendered blocks onto prefix_->blocks, and advance the fence-state tracker.  Shared
// between set_content() and append() — both need the exact same transition.
void StreamingMarkdown::commit_range(size_t boundary) {
    auto new_text = std::string_view{source_}.substr(committed_,
                                                     boundary - committed_);

    // Parse with our accumulated ref-def map as the resolution target, so
    // defs found in earlier commits are visible to links in this chunk, and
    // new defs found in this chunk become visible to future tail parses.
    std::string cleaned = collect_ref_defs(new_text, ref_defs_);
    RefDefsGuard guard(&ref_defs_);
    auto parsed = parse_markdown_impl(cleaned, 0);

    auto& prefix_blocks = prefix_->blocks;
    prefix_blocks.reserve(prefix_blocks.size() + parsed.blocks.size());
    for (auto& block : parsed.blocks) {
        // Heap-allocate the rendered Element and stash a shared_ptr.
        // Subsequent prefix generations reference the same heap
        // Elements (no move/copy of the BoxElement body when the
        // blocks vector reallocates past capacity).
        prefix_blocks.push_back(
            std::make_shared<const Element>(md_block_to_element(block)));
    }
    if (!parsed.blocks.empty()) ++prefix_->generation;

    for (size_t j = committed_; j < boundary; ++j) {
        bool at_line_start = (j == 0 || source_[j - 1] == '\n');
        if (!at_line_start) continue;
        bool is_code = j + 3 <= boundary &&
            ((source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') ||
             (source_[j] == '~' && source_[j+1] == '~' && source_[j+2] == '~'));
        bool is_math = !is_code && j + 2 <= boundary &&
            source_[j] == '$' && source_[j+1] == '$';
        if (is_code || is_math) in_code_fence_ = !in_code_fence_;
    }

    committed_ = boundary;
    build_dirty_ = true;

    // Keep scanner state coherent with the new committed_:
    //   • scan_last_boundary_ has just been "consumed" — anchor it to
    //     committed_ so the next find call doesn't re-return the same
    //     boundary as if it were still pending.
    //   • finish() can call commit_range(source_.size()) which jumps
    //     committed_ past scan_cursor_; in that case the scanner needs
    //     to be repositioned to committed_ and its fence parity
    //     resynchronised with in_code_fence_ (which the loop above
    //     just updated to the parity at boundary). For the normal
    //     append_safe path (boundary == scan_last_boundary_ ≤
    //     scan_cursor_) this branch is a no-op.
    scan_last_boundary_ = committed_;
    if (scan_cursor_ < committed_) {
        scan_cursor_   = committed_;
        scan_in_fence_ = in_code_fence_;
    }
}

// Internal: append codepoint-clean bytes that have already passed through
// the StreamSink.  Public entry points all funnel here.
void StreamingMarkdown::append_safe(std::string_view safe_bytes) {
    if (safe_bytes.empty()) return;
    source_.append(safe_bytes.data(), safe_bytes.size());
    build_dirty_ = true;

    size_t boundary = find_block_boundary();
    if (boundary > committed_) commit_range(boundary);
}

void StreamingMarkdown::set_content(std::string_view content) {
    // Fast-path: unchanged → no work. This dominates because the view layer
    // calls set_content(streaming_text) every single frame.
    if (content.size() == source_.size() &&
        (content.empty() || std::memcmp(content.data(), source_.data(),
                                        content.size()) == 0)) {
        return;
    }

    // Growth with identical prefix → append only the new bytes through the
    // StreamSink so partial-codepoint suffixes are buffered safely.  This
    // dominates real-world streaming usage (each frame's set_content is
    // the previous frame's content + a few new bytes).
    if (content.size() > source_.size() &&
        (source_.empty() || std::memcmp(content.data(), source_.data(),
                                        source_.size()) == 0)) {
        std::string_view delta{content.data() + source_.size(),
                               content.size() - source_.size()};
        std::string safe = sink_.feed(delta);
        if (!safe.empty()) append_safe(safe);
        return;
    }

    // Replace path: the new content diverges from the old prefix, so we
    // can't just append.  Reset and feed everything through the sink to
    // keep the codepoint-integrity guarantee.
    clear();
    std::string safe = sink_.feed(content);
    if (!safe.empty()) append_safe(safe);
}

void StreamingMarkdown::append(std::string_view text) {
    if (text.empty()) return;
    // Route every incoming chunk through the sink so the source_ buffer
    // never contains a half-written multi-byte codepoint or ANSI escape.
    // Anything mid-sequence is held in the sink's carry buffer until its
    // continuation arrives in a later append.
    std::string safe = sink_.feed(text);
    if (!safe.empty()) append_safe(safe);
}

void StreamingMarkdown::finish() {
    // Drain any held tail bytes from the sink before committing.  At
    // end-of-stream we accept that a half-decoded codepoint may surface
    // (it's better than dropping bytes silently); the renderer will
    // display invalid bytes as the U+FFFD replacement glyph.
    std::string pending = sink_.flush();
    if (!pending.empty()) append_safe(pending);

    if (committed_ < source_.size()) {
        commit_range(source_.size());
        in_code_fence_ = false;
    }
}

void StreamingMarkdown::clear() {
    source_.clear();
    committed_ = 0;
    // Replace the prefix snapshot rather than mutating the existing one
    // — any ComponentElement still capturing the old shared_ptr (held
    // inside cached_build_ until that's reassigned below) sees the
    // pre-clear content, and the next build() will allocate a fresh
    // prefix_ + ComponentElement so the renderer's component_cache
    // misses cleanly on the new instance instead of holding stale
    // entries against an unrelated subsequent stream.
    prefix_ = std::make_shared<CommittedPrefix>();
    ref_defs_.clear();
    in_code_fence_ = false;
    sink_.reset();
    build_dirty_ = true;
    cached_tail_size_ = 0;
    // Reset the resumable boundary scanner.
    scan_cursor_        = 0;
    scan_in_fence_      = false;
    scan_last_boundary_ = 0;
    // Reset the inline-tail cache. Strictly speaking the prefix-match
    // check at render_tail entry is self-correcting (any new tail will
    // mismatch and re-parse), but clearing here avoids carrying KB of
    // stale string state past a logical reset.
    tail_inline_cache_prefix_.clear();
    tail_inline_cache_content_.clear();
    tail_inline_cache_runs_.clear();
    // Reset the build-cache shape flags so the next build() falls into
    // the full-rebuild path rather than trying to mutate a stale
    // structural template.
    cached_prefix_gen_    = 0;
    cached_has_tail_      = false;
    cached_has_prefix_    = false;
    cached_tail_in_fence_ = false;
    cached_tail_bytes_.clear();
    cached_build_         = Element{TextElement{""}};
}

// Render the uncommitted tail as a monotonic in-progress paragraph.
//
// MONOTONICITY PROOF: this function never produces an element whose
// height *decreases* as bytes are appended to `tail`.
//
//   - Inside an open code fence: we render the tail as a literal text
//     element with TextWrap::Wrap, so its height is ceil(byte_count / W)
//     for terminal width W — strictly nondecreasing in byte_count.
//
//   - Otherwise: we parse `tail` as INLINE-only markdown (no block
//     recognition) via parse_inlines() and render through the same
//     wrapped-paragraph builder (build_inline_row).  Inline parse
//     produces a flat sequence of styled text spans whose total
//     character count is monotonic in byte_count.  Block markers at
//     line starts (|, -, *, >, #, etc.) are invisible to parse_inlines
//     and render as literal characters — so the tail never reflows
//     between "paragraph" and "table" / "list" / "blockquote" mid-stream.
//
//   - The block-level interpretation only happens when find_block_boundary
//     advances past the tail (on a blank line or closing fence), at
//     which point commit_range() moves the bytes into prefix_->blocks as a
//     properly-formatted Element and a new (empty) tail begins.  The
//     formatting "snap" is one-time per block and lands at a moment
//     when the inline diff sees a single coherent transition.
//
// This is the layer that makes ghosting structurally impossible for
// streaming markdown — there is no frame in which the renderer is
// asked to compare two element trees of different shape.
Element StreamingMarkdown::render_tail(std::string_view tail) const {
    // Skip leading newlines (whitespace from a prior commit boundary).
    std::size_t start = 0;
    while (start < tail.size() && tail[start] == '\n') ++start;
    if (start >= tail.size()) {
        return Element{TextElement{}};
    }
    std::string_view body = tail.substr(start);

    if (in_code_fence_) {
        // Inside an open code fence: render the literal bytes (including
        // the opening fence marker if it's still in this tail).  Wrap
        // mode keeps height monotonic in byte count.
        return Element{TextElement{
            .content = std::string{body},
            .style   = Style{}.with_fg(colors::code_fg),
            .wrap    = TextWrap::Wrap,
        }};
    }

    // ── Open code fence at tail start: render in its committed shape
    //    (round border + language label + syntax-highlighted body) so
    //    the user sees the formatted code block from the moment the
    //    opening ``` arrives, not after the closing ``` lands.  The
    //    closing fence hasn't arrived (find_block_boundary would have
    //    committed past it), so the tail is a verbatim opener line +
    //    in-progress body.  Monotonic in byte count: body grows row by
    //    row inside the same border frame.
    if (body.size() >= 3 &&
        ((body[0] == '`' && body[1] == '`' && body[2] == '`') ||
         (body[0] == '~' && body[1] == '~' && body[2] == '~')))
    {
        auto eol = body.find('\n');
        std::string lang;
        std::string code;
        if (eol == std::string_view::npos) {
            // Opener line not yet terminated: treat the post-fence
            // characters as the language hint and the body as empty.
            // The ``` itself is hidden so the row doesn't flicker the
            // marker bytes before the line break arrives.
            std::string_view lsv = body.substr(3);
            while (!lsv.empty() && (lsv.back() == ' ' || lsv.back() == '\r'))
                lsv.remove_suffix(1);
            while (!lsv.empty() && lsv.front() == ' ')
                lsv.remove_prefix(1);
            lang = std::string{lsv};
        } else {
            std::string_view lsv = body.substr(3, eol - 3);
            while (!lsv.empty() && (lsv.back() == ' ' || lsv.back() == '\r'))
                lsv.remove_suffix(1);
            while (!lsv.empty() && lsv.front() == ' ')
                lsv.remove_prefix(1);
            lang = std::string{lsv};
            code = std::string{body.substr(eol + 1)};
        }
        // Match md_block_to_element's CodeBlock styling exactly — same
        // builder shape, same align_self(Stretch) so the in-flight tail
        // render and the committed block render produce identical
        // borders. Without the stretch the streaming-tail border tracks
        // the longest emitted line, drifting frame-to-frame.
        auto builder = detail::vstack()
            .align_self(Align::Stretch)
            .border(BorderStyle::Round)
            .border_color(colors::code_border)
            .padding(0, 1, 0, 1);
        if (!lang.empty()) {
            std::string label = " " + lang + " ";
            if (lang == "mermaid" || lang == "matrix" ||
                lang == "math"    || lang == "latex"  ||
                lang == "dot"     || lang == "graphviz")
            {
                label = " " + lang + " (diagram) ";
            }
            builder = std::move(builder).border_text(
                std::move(label),
                BorderTextPos::Top,
                BorderTextAlign::Start);
        }
        return builder(highlight_code(code, lang));
    }

    // ── ATX heading at tail start: render the first line in its
    //    committed shape (bold + heading colour, hash markers stripped)
    //    so the heading style appears as soon as `# ` is typed instead
    //    of waiting for the next blank line to commit.  Monotonic: the
    //    heading line grows by one character per byte (single-line
    //    height stays at 1, multi-line wrap only ADDS rows).  Any
    //    follow-up content stays as inline parse below — promoting it
    //    to block parse would break monotonicity (list/table/quote
    //    snapping in/out of shape causes ghosting).
    int hashes = 0;
    while (hashes < 6 && static_cast<size_t>(hashes) < body.size() &&
           body[hashes] == '#') ++hashes;
    if (hashes > 0 && static_cast<size_t>(hashes) < body.size() &&
        body[hashes] == ' ')
    {
        auto eol = body.find('\n');
        std::string_view first = (eol == std::string_view::npos)
                                ? body
                                : body.substr(0, eol);
        std::string_view rest  = (eol == std::string_view::npos)
                                ? std::string_view{}
                                : body.substr(eol + 1);
        std::string_view text  = first.substr(static_cast<size_t>(hashes) + 1);

        Style sty = Style{}.with_bold();
        switch (hashes) {
            case 1: sty = sty.with_fg(colors::heading1); break;
            case 2: sty = sty.with_fg(colors::heading2); break;
            case 3: sty = sty.with_fg(colors::heading3); break;
            default: sty = sty.with_fg(colors::heading3).with_dim(); break;
        }
        Element heading_el = Element{TextElement{
            .content = std::string{text},
            .style   = sty,
        }};
        while (!rest.empty() && rest.front() == '\n') rest.remove_prefix(1);
        if (rest.empty()) return heading_el;
        auto spans = parse_inlines(rest);
        return detail::vstack().gap(1)(
            std::move(heading_el),
            build_inline_row(spans)
        );
    }

    // Inline-only parse for the rest — block markers (|, -, *, >) stay
    // literal until a real boundary advances past them.  This is the
    // monotonicity floor: a paragraph can only GROW row count as bytes
    // arrive, never shrink, so the renderer never has to clear stale
    // rows and ghosting is structurally impossible.  Lists / tables /
    // blockquotes pay a one-frame "snap" when the trailing blank line
    // commits the whole thing as one cohesive block.
    //
    // ── Sliding cache: parse only the live line ──
    //
    // Split body at the last '\n'. The "stable prefix" (everything
    // through and including the last '\n') is cache-keyed by its bytes;
    // the "live line" (post-newline tail) is re-parsed every frame.
    // Markdown emphasis / code-span / link delimiters never cross
    // newline boundaries in well-formed input — when the model emits
    // `**bold**\nNext line`, the parser state at the '\n' is "fresh,
    // no open delimiters" — so the split is parser-state-safe.
    //
    // Cost transition: prior code re-parsed the full body (O(tail)
    // per frame). Now per-frame work is O(live_line_length) on the
    // common case, with an O(tail) refresh once per newline boundary
    // (rare: bytes arrive faster than line breaks during streaming).
    auto last_nl = body.rfind('\n');
    std::string_view stable_prefix = (last_nl == std::string_view::npos)
        ? std::string_view{}
        : body.substr(0, last_nl + 1);
    std::string_view live_line = (last_nl == std::string_view::npos)
        ? body
        : body.substr(last_nl + 1);

    // Refresh the prefix cache only if it differs from what's stored.
    // String compare is O(prefix_len) bytewise but allocation-free —
    // orders of magnitude cheaper than re-running parse_inlines on
    // the same bytes.
    if (tail_inline_cache_prefix_.size() != stable_prefix.size()
        || std::string_view{tail_inline_cache_prefix_} != stable_prefix) {
        tail_inline_cache_content_.clear();
        tail_inline_cache_runs_.clear();
        if (!stable_prefix.empty()) {
            auto prefix_spans = parse_inlines(stable_prefix);
            const Style base = Style{}.with_fg(colors::text);
            for (const auto& s : prefix_spans) {
                flatten_inline(s, base, tail_inline_cache_content_,
                               tail_inline_cache_runs_);
            }
        }
        tail_inline_cache_prefix_.assign(stable_prefix);
    }

    // Build the result: cached prefix runs + freshly-parsed live-line
    // runs. flatten_inline writes run offsets relative to `out.size()`
    // at write time, so seeding `content` / `runs` from the cache
    // keeps the appended-line offsets globally correct.
    std::string content                  = tail_inline_cache_content_;
    std::vector<StyledRun> runs          = tail_inline_cache_runs_;
    if (!live_line.empty()) {
        auto live_spans = parse_inlines(live_line);
        const Style base = Style{}.with_fg(colors::text);
        for (const auto& s : live_spans) {
            flatten_inline(s, base, content, runs);
        }
    }

    if (runs.empty()) return Element{TextElement{}};
    if (runs.size() == 1) {
        return Element{TextElement{
            .content = std::move(content),
            .style   = runs[0].style,
        }};
    }
    return Element{TextElement{
        .content = std::move(content),
        .style   = Style{}.with_fg(colors::text),
        .runs    = std::move(runs),
    }};
}

const Element& StreamingMarkdown::build() const {
    // Untouched-since-last-build: return cached. Dominant case when the
    // widget is idle (no streaming).
    if (!build_dirty_) return cached_build_;

    std::string_view tail = (committed_ < source_.size())
        ? std::string_view{source_}.substr(committed_)
        : std::string_view{};
    const bool has_tail   = !tail.empty();
    const bool has_prefix = !prefix_->blocks.empty();

    // ── Empty special case ──
    if (!has_prefix && !has_tail) {
        cached_build_      = Element{TextElement{""}};
        cached_tail_size_  = 0;
        cached_prefix_gen_ = prefix_->generation;
        cached_has_tail_   = false;
        cached_has_prefix_ = false;
        build_dirty_       = false;
        return cached_build_;
    }

    // ── Tail-only fast path ──
    //
    // The hot streaming case: prefix unchanged, has_tail/has_prefix
    // shape unchanged, only the tail's content moved. We can mutate
    // cached_build_'s tail child in place — children[0] (the prefix
    // ComponentElement) keeps its address, the renderer's
    // component_cache stays warm, and only render_tail's lightweight
    // inline-cache path runs.
    //
    // Sub-fast-path: if the tail body bytes AND the committed-side
    // fence parity are byte-identical to the cache, render_tail's
    // output is provably identical to what's already in
    // children.back() — skip the call and the assignment entirely,
    // leaving cached_build_ wholly untouched. The bytes-equality check
    // matters for the case where build_dirty_ flipped (e.g., a feed
    // arrived) but the resulting tail body didn't actually change
    // (commit_range absorbed the new bytes; the body shape happened
    // to round-trip; any future code path that bumps build_dirty_
    // without changing the tail).
    if (cached_prefix_gen_ == prefix_->generation
        && cached_has_prefix_ == has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t expected = (has_prefix ? 1u : 0u) + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            if (has_tail) {
                const bool tail_unchanged =
                    cached_tail_in_fence_ == in_code_fence_
                    && cached_tail_bytes_.size() == tail.size()
                    && (tail.empty()
                        || std::memcmp(cached_tail_bytes_.data(),
                                       tail.data(), tail.size()) == 0);
                if (!tail_unchanged) {
                    // Swap in the new tail. children.back() is the
                    // tail slot regardless of whether prefix is
                    // present:
                    //   [prefix, tail] → children[1]
                    //   [tail]         → children[0]
                    box.children.back() = render_tail(tail);
                    cached_tail_bytes_.assign(tail);
                    cached_tail_in_fence_ = in_code_fence_;
                }
            }
            cached_tail_size_ = tail.size();
            build_dirty_      = false;
            return cached_build_;
        }
    }

    // ── Full rebuild ──
    //
    // Either the prefix generation moved (commit_range fired), the
    // shape (has_prefix / has_tail) changed, or the cache was reset.
    // Build the outer vstack with up to two children:
    //   children[0] = prefix ComponentElement (when has_prefix)
    //   children[last] = tail element (when has_tail)
    //
    // The prefix is wrapped in a ComponentElement whose render() pulls
    // from the captured shared_ptr<CommittedPrefix>. Its address lives
    // inside cached_build_'s children[0] — stable across frames until
    // we hit this rebuild path again — so the renderer's
    // component_cache can amortise the per-block layout + paint
    // across all subsequent frames at the same width.
    std::vector<Element> outer_children;
    outer_children.reserve(2);

    if (has_prefix) {
        // Capture by value: the lambda outlives any single build() call
        // (it's owned by cached_build_), and prefix_ might be replaced
        // by clear()/set_content's reset path.
        auto p = prefix_;
        ComponentElement comp;
        comp.render = [p](int /*w*/, int /*h*/) -> Element {
            // Materialise the inner vstack on cache miss. Each
            // shared_ptr<const Element> is dereferenced once into a
            // value-typed Element (the variant body is moved into the
            // children vector slot — no recursive deep clone of the
            // BoxElement subtree, just a pointer-swap of its inner
            // vector<Element>). The shared_ptr keeps the heap Element
            // alive while this output lives in the renderer's
            // component_cache; subsequent cache hits skip this whole
            // function. Inner vstack carries gap=1 (matching the
            // pre-component cadence between adjacent blocks); outer
            // vstack provides the left-pad and the prefix-vs-tail gap.
            std::vector<Element> kids;
            kids.reserve(p->blocks.size());
            for (const auto& sp : p->blocks) {
                kids.push_back(*sp);
            }
            return detail::vstack().gap(1)(std::move(kids)).build();
        };
        outer_children.push_back(Element{std::move(comp)});
    }
    if (has_tail) {
        outer_children.push_back(render_tail(tail));
    }

    // align_self(Stretch) forces this vstack to claim the parent's full
    // cross-axis size, regardless of how much its OWN children would
    // naturally span. Without this, early streaming frames where the
    // tail is a short inline-only paragraph (one wrapped row or less)
    // produce a vstack whose natural width = tail length = short, and
    // any parent header doing justify-content: space-between against
    // that width packs its children to the left. Setting Stretch makes
    // the streaming widget's width invariant across the
    // short-tail → multi-row-tail → committed-blocks transitions —
    // the parent's flex math sees the same number every frame, so the
    // header layout doesn't flicker between content-sized and
    // terminal-sized.
    cached_build_ = (
        detail::vstack().gap(1).padding(0, 0, 0, 2)
            .align_self(Align::Stretch)(std::move(outer_children))
    ).build();
    cached_tail_size_     = tail.size();
    cached_prefix_gen_    = prefix_->generation;
    cached_has_tail_      = has_tail;
    cached_has_prefix_    = has_prefix;
    cached_tail_in_fence_ = in_code_fence_;
    if (has_tail) cached_tail_bytes_.assign(tail);
    else          cached_tail_bytes_.clear();
    build_dirty_          = false;
    return cached_build_;
}

} // namespace maya
