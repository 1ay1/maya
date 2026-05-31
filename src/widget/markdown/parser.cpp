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
#include <list>
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
#include "maya/widget/markdown/spec_chars.hpp"

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
// Membership lives in one place — markdown/spec_chars.hpp — shared with
// the syntax highlighter and proven via static_assert there.
namespace chars = md_detail::chars;
using md_detail::chars::kEscapable;
using md_detail::chars::kInlineSpecial;

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

// ── HTML entity / emoji / URL / mention helpers ───────────────────────────
// Extracted to text_transform.cpp (the Text-node post-pass). Nothing in the
// structural inline/block parser below references them directly.

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

// ── Text-node post-pass (entities/emoji/URLs/mentions) ────────────────────
// Extracted to text_transform.cpp; declared in internal.hpp as
// md_detail::post_process_text_nodes. Used by parse_inlines() below.
using ::maya::md_detail::post_process_text_nodes;

// ============================================================================
// Emphasis: CommonMark delimiter-stack algorithm
// ============================================================================
// The inline scanner emits a flat token stream; runs of `*` / `_` are
// recorded as delimiter runs with left/right-flanking flags. A second pass
// (emph::process) pairs openers with closers honoring the rule-of-three,
// wrapping the spanned tokens into Bold / Italic. This is the reference cmark
// algorithm, replacing the old greedy find_closing heuristic.
namespace emph {

// Byte-level character class for flanking. At byte granularity: ASCII
// whitespace → Ws, ASCII punctuation → Punct, everything else (ASCII alnum
// + all UTF-8 bytes) → Other (letter-like).
enum class CClass : std::uint8_t { Ws, Punct, Other };

[[nodiscard]] inline CClass classify(char c) noexcept {
    if (chars::is_ascii_ws(c)) return CClass::Ws;
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x80 && chars::is_ascii_punct(c)) return CClass::Punct;
    return CClass::Other;
}

struct Flank { bool left; bool right; };

// Flanking + intraword `_` rules, CommonMark §6.2.
[[nodiscard]] inline Flank flanking(char delim, CClass before, CClass after) noexcept {
    const bool left_flank =
        after != CClass::Ws &&
        (after != CClass::Punct || before == CClass::Ws || before == CClass::Punct);
    const bool right_flank =
        before != CClass::Ws &&
        (before != CClass::Punct || after == CClass::Ws || after == CClass::Punct);
    Flank f{};
    if (delim == '_') {
        f.left  = left_flank  && (!right_flank || before == CClass::Punct);
        f.right = right_flank && (!left_flank  || after  == CClass::Punct);
    } else {
        f.left  = left_flank;
        f.right = right_flank;
    }
    return f;
}

// A token in the inline stream: a finished node, or an unresolved `*`/`_`
// delimiter run.
struct Tok {
    md::Inline node{md::Text{std::string{}}};
    bool  is_delim  = false;
    char  ch        = 0;
    int   count     = 0;   // remaining (unconsumed) delimiters
    int   orig      = 0;   // original run length — rule-of-3 reads this
    bool  can_open  = false;
    bool  can_close = false;
};

// Pair openers with closers, wrapping spanned tokens. Doubly-linked list so
// insert/erase keep neighbor iterators valid.
inline void process(std::list<Tok>& toks) {
    using It = std::list<Tok>::iterator;
    for (It closer = toks.begin(); closer != toks.end(); ++closer) {
        if (!closer->is_delim || !closer->can_close || closer->count == 0)
            continue;

        It opener = toks.end();
        if (closer != toks.begin()) {
            It j = closer;
            while (j != toks.begin()) {
                --j;
                if (!j->is_delim || j->ch != closer->ch) continue;
                if (!j->can_open || j->count == 0) continue;
                // Rule of three: forbid a pair whose summed ORIGINAL lengths
                // is a multiple of 3 when either run is both-sided, unless
                // both lengths are themselves multiples of 3.
                const bool either_both = closer->can_open || j->can_close;
                if (either_both &&
                    (j->orig + closer->orig) % 3 == 0 &&
                    !(j->orig % 3 == 0 && closer->orig % 3 == 0)) {
                    continue;
                }
                opener = j;
                break;
            }
        }
        if (opener == toks.end()) continue;

        const int use = (opener->count >= 2 && closer->count >= 2) ? 2 : 1;

        std::vector<md::Inline> children;
        for (It k = std::next(opener); k != closer; ) {
            if (k->is_delim) {
                if (k->count > 0)
                    children.push_back(md::Text{
                        std::string(static_cast<size_t>(k->count), k->ch)});
                k = toks.erase(k);
            } else {
                children.push_back(std::move(k->node));
                k = toks.erase(k);
            }
        }

        Tok wrapped;
        wrapped.node = (use == 2)
            ? md::Inline{md::Bold{std::move(children)}}
            : md::Inline{md::Italic{std::move(children)}};
        toks.insert(closer, std::move(wrapped));

        opener->count -= use;
        closer->count -= use;

        // Closer may still carry delimiters (e.g. `***a** b*`): revisit it.
        if (closer->count > 0) --closer;
    }
}

} // namespace emph

// Core inline parser. Emits a token stream (finished nodes + unresolved
// emphasis delimiter runs), resolves emphasis via the CommonMark
// delimiter-stack algorithm, then flattens to a node vector. Does NOT run
// the Text post-pass — the public parse_inlines wrapper does that once,
// recursing into emphasis children.
static std::vector<md::Inline> parse_inlines_impl(std::string_view text) {
    std::list<emph::Tok> toks;
    size_t i = 0;

    // Token-pushing helpers — coalesce adjacent literal text into the last
    // non-delimiter token, mirroring the old push_text/push_char behavior.
    auto push_node = [&](md::Inline n) {
        toks.push_back(emph::Tok{std::move(n)});
    };
    auto push_text = [&](std::string_view sv) {
        if (sv.empty()) return;
        if (!toks.empty() && !toks.back().is_delim) {
            if (auto* t = std::get_if<md::Text>(&toks.back().node.inner)) {
                t->content.append(sv);
                return;
            }
        }
        push_node(md::Text{std::string{sv}});
    };
    auto push_char = [&](char c) { push_text(std::string_view{&c, 1}); };

    while (i < text.size()) {
        // Backslash escape
        if (text[i] == '\\') {
            if (i + 1 < text.size() && is_escapable(text[i + 1])) {
                push_char(text[i + 1]);
                i += 2;
                continue;
            }
            // Hard line break: backslash before newline
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                push_node(md::HardBreak{});
                i += 2;
                continue;
            }
            // Trailing or unrecognized backslash — emit as a literal so we
            // always advance.
            push_text(text.substr(i, 1));
            ++i;
            continue;
        }

        // Hard line break: two+ trailing spaces before newline
        if (text[i] == ' ' && i + 2 < text.size()) {
            size_t spaces = 0;
            size_t j = i;
            while (j < text.size() && text[j] == ' ') { ++spaces; ++j; }
            if (spaces >= 2 && j < text.size() && text[j] == '\n') {
                push_node(md::HardBreak{});
                i = j + 1;
                continue;
            }
        }

        // Inline code: `code` or ``code``
        if (text[i] == '`') {
            size_t ticks = 0;
            size_t j = i;
            while (j < text.size() && text[j] == '`') { ++ticks; ++j; }
            auto closing = std::string(ticks, '`');
            size_t end = text.find(std::string_view{closing}, j);
            if (end != std::string_view::npos) {
                auto code = text.substr(j, end - j);
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') {
                    code.remove_prefix(1);
                    code.remove_suffix(1);
                }
                push_node(md::Code{std::string{code}});
                i = end + ticks;
                continue;
            }
            push_text(text.substr(i, ticks));
            i = j;
            continue;
        }

        // Math: $$...$$ (display) or $...$ (inline) — render as code.
        if (text[i] == '$') {
            if (i + 1 < text.size() && text[i + 1] == '$') {
                size_t end = text.find("$$", i + 2);
                if (end != std::string_view::npos) {
                    auto content = text.substr(i + 2, end - i - 2);
                    push_node(md::Code{std::string{content}});
                    i = end + 2;
                    continue;
                }
            }
            if (i + 1 < text.size() && text[i + 1] != ' ' && text[i + 1] != '$') {
                size_t end = text.find('$', i + 1);
                if (end != std::string_view::npos && end > i + 1 &&
                    text[end - 1] != ' ') {
                    auto content = text.substr(i + 1, end - i - 1);
                    push_node(md::Code{std::string{content}});
                    i = end + 1;
                    continue;
                }
            }
            push_text("$");
            ++i;
            continue;
        }

        // Strikethrough: ~~text~~  (GFM extension — greedy match)
        if (i + 1 < text.size() && text[i] == '~' && text[i + 1] == '~') {
            size_t end = find_closing(text, "~~", i + 2);
            if (end != std::string_view::npos) {
                auto inner = text.substr(i + 2, end - i - 2);
                push_node(md::Strike{parse_inlines_impl(inner)});
                i = end + 2;
                continue;
            }
            push_text("~~");
            i += 2;
            continue;
        }

        // Subscript: ~text~ (single ~) — after strike (~~) check.
        if (text[i] == '~') {
            if (i + 1 < text.size() && text[i + 1] != '~' && text[i + 1] != ' ') {
                size_t scan_limit = std::min(text.size(), i + 1 + 200);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < scan_limit; ++s) {
                    if (text[s] == '~') { end = s; break; }
                    if (text[s] == ' ' || text[s] == '\n') break;
                }
                if (end != std::string_view::npos && text[end - 1] != ' ') {
                    auto inner = text.substr(i + 1, end - i - 1);
                    push_node(md::Sub{parse_inlines_impl(inner)});
                    i = end + 1;
                    continue;
                }
            }
        }

        // Highlight: ==text== (maya extension / PHP Markdown Extra).
        if (i + 1 < text.size() && text[i] == '=' && text[i + 1] == '=') {
            size_t end = find_closing(text, "==", i + 2);
            if (end != std::string_view::npos) {
                auto inner = text.substr(i + 2, end - i - 2);
                push_node(md::Highlight{parse_inlines_impl(inner)});
                i = end + 2;
                continue;
            }
            push_text(text.substr(i, 1));
            ++i;
            continue;
        }
        if (text[i] == '=') {
            push_text(text.substr(i, 1));
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
                    push_node(md::Sup{parse_inlines_impl(inner)});
                    i = end + 1;
                    continue;
                }
            }
            push_text(text.substr(i, 1));
            ++i;
            continue;
        }

        // Emphasis: record a `*` / `_` delimiter run; emph::process pairs
        // them in a second pass per CommonMark §6.2.
        if (text[i] == '*' || text[i] == '_') {
            char d = text[i];
            size_t run = 1;
            while (i + run < text.size() && text[i + run] == d) ++run;
            // Delimiter-run cap. A run of dozens of identical `*`/`_` is
            // never real emphasis — and a single huge opener paired with a
            // single huge closer would generate ~run/2-deep nesting, which
            // the per-frame streaming re-parse renders super-linearly
            // (depth 500 ≈ 5 s, depth 1000 hangs). Cap the run used for
            // emphasis at kMaxEmphRun; emit any excess as literal text.
            // Real markdown / every CommonMark spec example uses runs of
            // 1–6, so this is invisible to correct input and bounds the
            // pathological case to O(kMaxEmphRun) nesting.
            constexpr size_t kMaxEmphRun = 64;
            if (run > kMaxEmphRun) {
                push_text(text.substr(i, run - kMaxEmphRun));
                i += run - kMaxEmphRun;
                run = kMaxEmphRun;
            }
            emph::CClass before = (i == 0)
                ? emph::CClass::Ws : emph::classify(text[i - 1]);
            emph::CClass after = (i + run >= text.size())
                ? emph::CClass::Ws : emph::classify(text[i + run]);
            emph::Flank fl = emph::flanking(d, before, after);
            emph::Tok t;
            t.is_delim  = true;
            t.ch        = d;
            t.count     = static_cast<int>(run);
            t.orig      = static_cast<int>(run);
            t.can_open  = fl.left;
            t.can_close = fl.right;
            toks.push_back(std::move(t));
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
                        push_node(md::Image{
                            std::string{alt}, std::move(url), std::move(title)});
                        i = pos;
                        continue;
                    }
                } else if (after < text.size() && text[after] == '[') {
                    size_t pos = after;
                    auto* ref = parse_link_ref(text, pos, alt);
                    if (ref) {
                        push_node(md::Image{
                            std::string{alt}, ref->url, ref->title});
                        i = pos;
                        continue;
                    }
                } else {
                    // Shortcut ![alt] — look up `alt` directly.
                    if (auto* ref = lookup_ref(alt)) {
                        push_node(md::Image{
                            std::string{alt}, ref->url, ref->title});
                        i = close_bracket + 1;
                        continue;
                    }
                }
            }
            push_text("!");
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
                        push_node(md::FootnoteRef{std::string{label}});
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
                        push_node(md::Link{
                            std::string{link_text}, std::move(url), std::move(title)});
                        i = pos;
                        continue;
                    }
                } else if (after < text.size() && text[after] == '[') {
                    size_t pos = after;
                    if (auto* ref = parse_link_ref(text, pos, link_text)) {
                        push_node(md::Link{
                            std::string{link_text}, ref->url, ref->title});
                        i = pos;
                        continue;
                    }
                } else {
                    if (auto* ref = lookup_ref(link_text)) {
                        push_node(md::Link{
                            std::string{link_text}, ref->url, ref->title});
                        i = close_bracket + 1;
                        continue;
                    }
                }
            }
            push_text("[");
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
                    push_node(md::HardBreak{});
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
                    auto inner = parse_inlines_impl(body);
                    if (!tag.attr_href.empty()) {
                        std::string tx;
                        for (auto& sp : inner)
                            if (auto* t = std::get_if<md::Text>(&sp.inner)) tx += t->content;
                        push_node(md::Link{
                            std::move(tx), std::move(tag.attr_href), ""});
                    } else {
                        for (auto& sp : inner) push_node(std::move(sp));
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
                    auto inner = parse_inlines_impl(body);
                    wrap(std::move(inner), tag);
                    i = (closer == std::string_view::npos)
                        ? text.size()
                        : closer + 3 + std::string_view(name).size();
                    return true;
                };
                if (render_paired("strong", [&](auto inner, auto&) {
                    push_node(md::Bold{std::move(inner)});
                })) continue;
                if (render_paired("em", [&](auto inner, auto&) {
                    push_node(md::Italic{std::move(inner)});
                })) continue;
                if (render_paired("mark", [&](auto inner, auto&) {
                    push_node(md::Highlight{std::move(inner)});
                })) continue;
                if (render_paired("sub", [&](auto inner, auto&) {
                    push_node(md::Sub{std::move(inner)});
                })) continue;
                if (render_paired("sup", [&](auto inner, auto&) {
                    push_node(md::Sup{std::move(inner)});
                })) continue;
                if (render_paired("kbd", [&](auto inner, auto&) {
                    push_node(md::Kbd{std::move(inner)});
                })) continue;
                if (render_paired("span", [&](auto inner, auto&) {
                    for (auto& sp : inner) push_node(std::move(sp));
                })) continue;
                if (render_paired("abbr", [&](auto inner, auto& t) {
                    push_node(md::Abbr{std::move(t.attr_title), std::move(inner)});
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
                    push_node(md::Link{
                        std::string{content}, std::move(url_str), ""});
                    i = close + 1;
                    continue;
                }
            }
            push_text("<");
            ++i;
            continue;
        }

        // Unmatched special characters (! without [, lone ~)
        if (text[i] == '~' || text[i] == '!') {
            push_text(text.substr(i, 1));
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
            push_text(text.substr(start, i - start));
        }
    }

    // Resolve emphasis delimiter runs (CommonMark §6.2), then flatten the
    // token list to a node vector. Leftover delimiters degrade to literal
    // text. The Text post-pass runs once in the public wrapper.
    emph::process(toks);
    std::vector<md::Inline> result;
    result.reserve(toks.size());
    for (auto& tk : toks) {
        if (tk.is_delim) {
            if (tk.count > 0) {
                std::string lit(static_cast<size_t>(tk.count), tk.ch);
                if (!result.empty()) {
                    if (auto* t = std::get_if<md::Text>(&result.back().inner)) {
                        t->content += lit;
                        continue;
                    }
                }
                result.push_back(md::Text{std::move(lit)});
            }
        } else {
            result.push_back(std::move(tk.node));
        }
    }
    return result;
}

// Public inline entry: resolve inline structure, then run the Text post-pass
// (entities / emoji / bare-URL / mentions) once over the whole tree,
// recursing into emphasis children.
std::vector<md::Inline> parse_inlines(std::string_view text) {
    auto nodes = parse_inlines_impl(text);
    post_process_text_nodes(nodes);
    return nodes;
}

// ============================================================================
// Table helpers
// ============================================================================

// GFM table line predicates + cell splitting live in tables.cpp; pull them
// into this TU's lookup so the block parser below calls them unqualified.
using ::maya::md_detail::is_table_row;
using ::maya::md_detail::is_table_separator;
using ::maya::md_detail::split_table_cells;
using ::maya::md_detail::parse_table_alignments;

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

        // ATX Heading: up to 3 leading spaces, 1–6 `#`, then a space/tab or
        // end-of-line. CommonMark §4.2.
        if (size_t hs = static_cast<size_t>(count_indent(line)); hs < 4) {
            size_t j = hs;
            int level = 0;
            while (j < line.size() && line[j] == '#') { ++level; ++j; }
            // 1–6 hashes, and the opening sequence must be followed by a
            // space/tab or be the whole line (`##` alone is an empty h2).
            const bool valid_open =
                level >= 1 && level <= 6 &&
                (j >= line.size() || line[j] == ' ' || line[j] == '\t');
            if (valid_open) {
                flush_paragraph();
                // Trim leading spaces/tabs of the content.
                while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
                auto content = trim(line.substr(j));
                // Strip an optional closing sequence: spaces, then a run of
                // `#`, then trailing spaces — but only when the `#` run is
                // preceded by a space (or is the whole content). `foo#` keeps
                // its trailing hash.
                {
                    auto c = content;
                    while (!c.empty() && (c.back() == ' ' || c.back() == '\t'))
                        c.remove_suffix(1);
                    size_t hashes = 0;
                    while (hashes < c.size() && c[c.size() - 1 - hashes] == '#')
                        ++hashes;
                    if (hashes > 0) {
                        size_t before = c.size() - hashes;
                        if (before == 0 ||
                            c[before - 1] == ' ' || c[before - 1] == '\t') {
                            c.remove_suffix(hashes);
                            content = trim(c);
                        }
                    }
                }
                doc.blocks.push_back(md::Heading{level, parse_inlines(content)});
                ++i;
                continue;
            }
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
            bool ordered = ol_len > 0;
            int marker_len = ordered ? ol_len : ul_len;
            bool is_list = (ul_len > 0 || ol_len > 0);
            // Paragraph interruption (CommonMark §5.2/§5.3): a list may
            // interrupt a paragraph only when it is a bullet list or an
            // ordered list starting at 1, AND the marker is followed by
            // non-blank content. Otherwise the line is lazy paragraph text
            // (e.g. "...is\n14. ..." stays one paragraph).
            if (is_list && !paragraph_buf.empty()) {
                bool content_blank =
                    trim(line.substr(static_cast<size_t>(marker_len))).empty();
                int sn = ordered ? ol_start_num(line) : 1;
                if (content_blank || (ordered && sn != 1)) is_list = false;
            }
            if (is_list) {
                flush_paragraph();
                int start_num = ordered ? ol_start_num(line) : 1;
                int base_indent = count_indent(line);
                // Marker delimiter byte: -,*,+ (bullet) or . ) (ordered).
                // For both forms it sits at marker_len-2 (= leading-ws +
                // optional digits). A sibling item must share this delimiter;
                // a different one ends the list and starts a new one (so
                // "- a\n+ b" and "1. a\n2) b" each split into two lists).
                char list_delim = line[static_cast<size_t>(marker_len) - 2];

                std::vector<md::ListItem> items;
                bool loose = false;  // CommonMark §5.3 loose-list detection

                while (i < lines.size()) {
                    auto ll = lines[i];
                    if (!ll.empty() && ll.back() == '\r') ll.remove_suffix(1);

                    int cur_ul = ul_marker_len(ll);
                    int cur_ol = ol_marker_len(ll);
                    int cur_marker_len = ordered ? cur_ol : cur_ul;
                    bool is_item = (ordered ? (cur_ol > 0) : (cur_ul > 0)) &&
                        cur_marker_len >= 2 &&
                        ll[static_cast<size_t>(cur_marker_len) - 2] == list_delim;
                    int cur_indent = count_indent(ll);

                    // Only match items at the same indentation level
                    if (!is_item || std::abs(cur_indent - base_indent) > 1) {
                        // Could be a continuation or sub-list
                        if (trim(ll).empty() || cur_indent > base_indent + 1) {
                            // Continuation or nested content — append to last item
                            if (!items.empty()) {
                                // Collect all continuation/nested lines
                                std::string sub_text;
                                bool had_blank = false;
                                while (i < lines.size()) {
                                    auto sl = lines[i];
                                    if (!sl.empty() && sl.back() == '\r') sl.remove_suffix(1);
                                    int si = count_indent(sl);
                                    bool blank = trim(sl).empty();
                                    if (blank) had_blank = true;

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
                                    bool added_block = false;
                                    for (auto& b : sub_doc.blocks) {
                                        // A Paragraph child separated from the item's
                                        // first line by a blank makes the list loose
                                        // (item directly holds two block elements).
                                        if (std::holds_alternative<md::Paragraph>(b.inner))
                                            added_block = true;
                                        items.back().children.push_back(std::move(b));
                                    }
                                    if (had_blank && added_block) loose = true;
                                }
                                // A blank line directly before the next sibling
                                // marker makes the list loose (items separated by
                                // a blank line). Trailing blanks before a non-item
                                // line don't count.
                                if (had_blank && i < lines.size()) {
                                    auto nl = lines[i];
                                    if (!nl.empty() && nl.back() == '\r') nl.remove_suffix(1);
                                    int nml = ordered ? ol_marker_len(nl) : ul_marker_len(nl);
                                    if (nml >= 2 && count_indent(nl) == base_indent &&
                                        nl[static_cast<size_t>(nml) - 2] == list_delim)
                                        loose = true;
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
                doc.blocks.push_back(md::List{std::move(items), ordered, start_num, loose});
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
