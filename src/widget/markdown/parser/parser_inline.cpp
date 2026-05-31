// parser_inline.cpp — the inline parser (spans within paragraphs/headings).
//
// Owns: link destination/reference parsing, the CommonMark emphasis
// delimiter-stack (emph::), the single-pass inline scanner
// parse_inlines_impl, and the public parse_inlines wrapper (which runs the
// text post-pass once over the whole tree). Also DEFINES the reference-link
// TLS slot + RefDefsScope that the block parser and streaming TU share.

#include "maya/widget/markdown.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/widget/markdown/internal.hpp"
#include "maya/widget/markdown/spec_chars.hpp"

#include "maya/widget/markdown/parser_internal.hpp"

namespace maya {

// Reference-link map (thread_local) — defined here, declared extern in
// parser_internal.hpp. parse_inlines stashes the active Document's ref_defs
// here via RefDefsScope so lookup_ref resolves [text][label] references.
namespace md_detail {
thread_local const std::unordered_map<std::string, md::LinkRef>*
    tls_ref_defs = nullptr;

RefDefsScope::RefDefsScope(const std::unordered_map<std::string, md::LinkRef>* p) noexcept
    : prev(tls_ref_defs) { tls_ref_defs = p; }
RefDefsScope::~RefDefsScope() { tls_ref_defs = prev; }
} // namespace md_detail

// Bring the shared small helpers (trim / starts_with / is_escapable /
// ascii_lower / lookup_ref / dedent / ol_start_num / parse_task_checkbox /
// find_eol) into scope so the verbatim bodies below compile unchanged.
using namespace ::maya::md_detail::parser_detail;
using ::maya::md_detail::chars::kInlineSpecial;

namespace {

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

// ── HTML tag scanner (shared inline + block) ──────────────────────────────
// HtmlTagInfo / try_parse_html_tag / find_html_closer live in html_tag.cpp
// (declared in internal.hpp). Pull them into this TU's lookup.
using ::maya::md_detail::HtmlTagInfo;
using ::maya::md_detail::try_parse_html_tag;
using ::maya::md_detail::find_html_closer;

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

} // anonymous namespace

std::vector<md::Inline> parse_inlines(std::string_view text) {
    auto nodes = parse_inlines_impl(text);
    post_process_text_nodes(nodes);
    return nodes;
}

// ============================================================================
// Table helpers
// ============================================================================


// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
std::vector<md::Inline> parse_inlines(std::string_view text) {
    return ::maya::parse_inlines(text);
}
} // namespace md_detail

} // namespace maya
