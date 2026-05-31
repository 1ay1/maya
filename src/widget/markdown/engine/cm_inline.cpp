// cm_inline.cpp — CommonMark inline-structure phase (spec §6).
//
// The delimiter-stack algorithm: scan the text left-to-right producing a
// flat list of inline nodes, pushing emphasis delimiters (* _) and link/
// image openers ([ ![) onto a stack; when we hit a ] or end of input we
// resolve links/images, then process emphasis by walking the delimiter
// stack pairing openers with closers under the flanking + multiple-of-3
// rules. Code spans, autolinks, raw HTML, entities, backslash escapes, and
// hard/soft line breaks are handled inline during the scan.
//
// Reference: https://spec.commonmark.org/0.31.2/#inlines

#include "maya/widget/markdown/engine/cm_engine.hpp"
#include "maya/widget/markdown/engine/cm_util.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace maya::md_detail::engine {
namespace {

// A node under construction. We keep a flat vector; emphasis resolution
// reaches back into it by index. Text nodes are mutable so emphasis can
// split them at delimiter runs.
struct Node {
    md::Inline span;
    // delimiter bookkeeping (only meaningful for delimiter-run text nodes)
    bool is_delim = false;
    char delim_char = 0;     // '*', '_', '[', '!'
    int  delim_count = 0;    // remaining usable delimiters
    bool can_open = false;
    bool can_close = false;
    bool active = true;      // for link openers
    bool is_image = false;   // for openers
    std::size_t orig_count = 0;
    int emph_depth = 0;       // emphasis nesting depth of this node

    Node() : span(md::Inline{md::Text{}}) {}
    explicit Node(md::Inline s) : span(std::move(s)) {}
};

class InlineParser {
public:
    explicit InlineParser(const RefMap& refs) : refs_(refs) {}

    std::vector<md::Inline> parse(std::string_view text) {
        text_ = text;
        scan();
        process_emphasis(0);
        // collect output, skipping fully-consumed delimiters
        std::vector<md::Inline> out;
        for (auto& n : nodes_) {
            if (n.is_delim && n.delim_count > 0) {
                // leftover delimiter run → literal text
                std::string lit(static_cast<std::size_t>(n.delim_count), n.delim_char);
                out.push_back(md::Text{std::move(lit)});
            } else if (n.is_delim) {
                // openers ([ !) that were never matched → literal
                if (n.delim_char == '[') out.push_back(md::Text{"["});
                else if (n.delim_char == '!') out.push_back(md::Text{"!["});
                // consumed emphasis delimiters contribute nothing
            } else {
                out.push_back(std::move(n.span));
            }
        }
        return merge_text(std::move(out));
    }

private:
    std::string_view text_;
    const RefMap& refs_;
    std::vector<Node> nodes_;

    void push_text(std::string s) {
        if (s.empty()) return;
        nodes_.push_back(Node{md::Inline{md::Text{std::move(s)}}});
    }
    void push_node(md::Inline n) {
        nodes_.push_back(Node{std::move(n)});
    }

    // ── main scan ────────────────────────────────────────────────────────
    void scan() {
        std::size_t i = 0;
        std::string pending;
        auto flush = [&] { if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); } };

        while (i < text_.size()) {
            char c = text_[i];
            switch (c) {
                case '\\': {
                    if (i + 1 < text_.size()) {
                        char n = text_[i + 1];
                        if (n == '\n') {
                            // hard break
                            flush();
                            push_node(md::HardBreak{});
                            i += 2;
                            // skip leading spaces of next line
                            while (i < text_.size() && text_[i] == ' ') ++i;
                            continue;
                        }
                        if (is_ascii_punct(n)) {
                            pending += n;
                            i += 2;
                            continue;
                        }
                    }
                    pending += '\\';
                    ++i;
                    continue;
                }
                case '`': {
                    std::size_t adv;
                    if (try_code_span(i, pending, adv)) { i = adv; continue; }
                    // No matching closing run: the ENTIRE opening run is
                    // literal (so ```foo`` doesn't spuriously match the
                    // trailing `` as a shorter span).
                    std::size_t run = 0;
                    while (i + run < text_.size() && text_[i + run] == '`') ++run;
                    pending.append(run, '`');
                    i += run;
                    continue;
                }
                case '<': {
                    std::size_t adv;
                    if (try_autolink_or_html(i, pending, adv)) { i = adv; continue; }
                    pending += '<';
                    ++i;
                    continue;
                }
                case '&': {
                    std::string dec;
                    std::size_t used = decode_entity(text_, i, dec);
                    if (used) { pending += dec; i += used; continue; }
                    pending += '&';
                    ++i;
                    continue;
                }
                case '\n': {
                    // soft or hard break: trailing >=2 spaces → hard
                    flush();
                    std::size_t sp = pending.size();
                    (void)sp;
                    // count trailing spaces already in pending? we flushed; so
                    // look back at text_ before the newline
                    std::size_t back = i;
                    int spaces = 0;
                    while (back > 0 && text_[back - 1] == ' ') { ++spaces; --back; }
                    if (spaces >= 2) {
                        // remove the trailing spaces from the last text node
                        trim_trailing_spaces();
                        push_node(md::HardBreak{});
                    } else {
                        trim_trailing_spaces();
                        push_node(md::SoftBreak{});
                    }
                    ++i;
                    // skip leading spaces of the continuation line
                    while (i < text_.size() && (text_[i] == ' ' || text_[i] == '\t')) ++i;
                    continue;
                }
                case '*':
                case '_': {
                    flush();
                    scan_delim_run(i, c);
                    continue;
                }
                case '[': {
                    flush();
                    Node n{md::Inline{md::Text{"["}}};
                    n.is_delim = true;
                    n.delim_char = '[';
                    nodes_.push_back(std::move(n));
                    ++i;
                    continue;
                }
                case '!': {
                    if (i + 1 < text_.size() && text_[i + 1] == '[') {
                        flush();
                        Node n{md::Inline{md::Text{"!["}}};
                        n.is_delim = true;
                        n.delim_char = '!';
                        n.is_image = true;
                        nodes_.push_back(std::move(n));
                        i += 2;
                        continue;
                    }
                    pending += '!';
                    ++i;
                    continue;
                }
                case ']': {
                    flush();
                    if (!try_close_link(i)) { pending += ']'; ++i; }
                    continue;
                }
                default:
                    pending += c;
                    ++i;
                    continue;
            }
        }
        flush();
    }

    void trim_trailing_spaces() {
        if (nodes_.empty()) return;
        Node& n = nodes_.back();
        if (n.is_delim) return;
        if (auto* t = std::get_if<md::Text>(&n.span.inner)) {
            while (!t->content.empty() && t->content.back() == ' ')
                t->content.pop_back();
            if (t->content.empty()) nodes_.pop_back();
        }
    }

    // ── code spans (§6.1) ────────────────────────────────────────────────
    bool try_code_span(std::size_t i, std::string& pending, std::size_t& adv) {
        std::size_t n = 0;
        while (i + n < text_.size() && text_[i + n] == '`') ++n;
        // find closing run of exactly n backticks
        std::size_t j = i + n;
        while (j < text_.size()) {
            if (text_[j] == '`') {
                std::size_t m = 0;
                while (j + m < text_.size() && text_[j + m] == '`') ++m;
                if (m == n) {
                    // content is text_[i+n .. j)
                    std::string content(text_.substr(i + n, j - (i + n)));
                    // line endings → spaces; strip single leading+trailing
                    for (char& ch : content) if (ch == '\n') ch = ' ';
                    if (content.size() >= 2 && content.front() == ' ' &&
                        content.back() == ' ' &&
                        content.find_first_not_of(' ') != std::string::npos) {
                        content = content.substr(1, content.size() - 2);
                    }
                    if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
                    push_node(md::Code{std::move(content)});
                    adv = j + m;
                    return true;
                }
                j += m;
            } else {
                ++j;
            }
        }
        return false;
    }

    // ── autolinks + raw HTML (§6.5, §6.6) ────────────────────────────────
    bool try_autolink_or_html(std::size_t i, std::string& pending, std::size_t& adv) {
        // absolute URI autolink: <scheme:...>
        std::size_t close = text_.find('>', i + 1);
        if (close == std::string_view::npos) return false;
        std::string_view inner = text_.substr(i + 1, close - (i + 1));
        if (inner.find(' ') == std::string_view::npos &&
            inner.find('<') == std::string_view::npos && !inner.empty()) {
            // scheme: letter, then 1-31 of (letter,digit,+,-,.) then ':'
            std::size_t colon = inner.find(':');
            if (colon != std::string_view::npos && colon >= 2 && colon <= 32 &&
                is_ascii_alpha(inner[0])) {
                bool good = true;
                for (std::size_t k = 1; k < colon; ++k) {
                    char ch = inner[k];
                    if (!is_ascii_alnum(ch) && ch != '+' && ch != '-' && ch != '.') {
                        good = false; break;
                    }
                }
                if (good) {
                    if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
                    md::Link l;
                    l.url = std::string(inner);
                    l.text = std::string(inner);
                    l.kids.push_back(md::Text{std::string(inner)});
                    push_node(std::move(l));
                    adv = close + 1;
                    return true;
                }
            }
            // email autolink
            if (is_email(inner)) {
                if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
                md::Link l;
                l.url = "mailto:" + std::string(inner);
                l.text = std::string(inner);
                l.kids.push_back(md::Text{std::string(inner)});
                push_node(std::move(l));
                adv = close + 1;
                return true;
            }
        }
        // raw inline HTML tag
        std::size_t htlen = scan_html_tag(i);
        if (htlen) {
            if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
            push_node(md::RawInline{std::string(text_.substr(i, htlen))});
            adv = i + htlen;
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool is_email(std::string_view s) {
        auto at = s.find('@');
        if (at == std::string_view::npos || at == 0 || at + 1 >= s.size())
            return false;
        for (std::size_t k = 0; k < at; ++k) {
            char c = s[k];
            if (!is_ascii_alnum(c) &&
                std::string_view(".!#$%&'*+/=?^_`{|}~-").find(c) == std::string_view::npos)
                return false;
        }
        // domain: labels of alnum/-, dot separated
        std::string_view dom = s.substr(at + 1);
        std::size_t start = 0;
        for (std::size_t k = 0; k <= dom.size(); ++k) {
            if (k == dom.size() || dom[k] == '.') {
                if (k == start) return false;
                start = k + 1;
            } else {
                char c = dom[k];
                if (!is_ascii_alnum(c) && c != '-') return false;
            }
        }
        return true;
    }

    // Scan a raw HTML tag starting at text_[i]=='<'. Returns length or 0.
    [[nodiscard]] std::size_t scan_html_tag(std::size_t i) const {
        std::string_view s = text_;
        std::size_t n = s.size();
        if (i >= n || s[i] != '<') return 0;
        std::size_t j = i + 1;
        // comment <!-- ... -->
        if (s.compare(j, 3, "!--") == 0) {
            std::size_t e = s.find("-->", j + 3);
            return e == std::string_view::npos ? 0 : (e + 3) - i;
        }
        // CDATA <![CDATA[ ... ]]>
        if (s.compare(j, 8, "![CDATA[") == 0) {
            std::size_t e = s.find("]]>", j + 8);
            return e == std::string_view::npos ? 0 : (e + 3) - i;
        }
        // processing instruction
        if (j < n && s[j] == '?') {
            std::size_t e = s.find("?>", j + 1);
            return e == std::string_view::npos ? 0 : (e + 2) - i;
        }
        // declaration <!NAME ...>
        if (j < n && s[j] == '!') {
            std::size_t e = s.find('>', j);
            return e == std::string_view::npos ? 0 : (e + 1) - i;
        }
        bool closing = (j < n && s[j] == '/');
        if (closing) ++j;
        if (j >= n || !is_ascii_alpha(s[j])) return 0;
        ++j;
        while (j < n && (is_ascii_alnum(s[j]) || s[j] == '-')) ++j;
        // attributes (only for open tags)
        if (!closing) {
            for (;;) {
                std::size_t ws = j;
                while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
                if (j < n && (s[j] == '>' || s[j] == '/')) break;
                if (j == ws) break;  // need whitespace before attr
                if (j >= n || !(is_ascii_alpha(s[j]) || s[j]=='_' || s[j]==':')) break;
                ++j;
                while (j < n && (is_ascii_alnum(s[j]) || s[j]=='_' || s[j]==':' ||
                                 s[j]=='.' || s[j]=='-')) ++j;
                // optional value
                std::size_t save = j;
                while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
                if (j < n && s[j] == '=') {
                    ++j;
                    while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
                    if (j < n && (s[j] == '"' || s[j] == '\'')) {
                        char q = s[j++];
                        while (j < n && s[j] != q) ++j;
                        if (j >= n) return 0;
                        ++j;
                    } else {
                        std::size_t vs = j;
                        while (j < n && s[j]!=' ' && s[j]!='\t' && s[j]!='\n' &&
                               s[j]!='\r' && s[j]!='"' && s[j]!='\'' && s[j]!='=' &&
                               s[j]!='<' && s[j]!='>' && s[j]!='`') ++j;
                        if (j == vs) return 0;
                    }
                } else {
                    j = save;  // no value
                }
            }
        } else {
            while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
        }
        if (j < n && s[j] == '/' && !closing) ++j;
        if (j >= n || s[j] != '>') return 0;
        return (j + 1) - i;
    }

    // ── emphasis delimiter runs (§6.2) ───────────────────────────────────
    void scan_delim_run(std::size_t& i, char c) {
        std::size_t start = i;
        while (i < text_.size() && text_[i] == c) ++i;
        int count = static_cast<int>(i - start);

        char prev = start > 0 ? text_[start - 1] : ' ';
        char next = i < text_.size() ? text_[i] : ' ';

        bool prev_ws = is_unicode_ws_byte(static_cast<unsigned char>(prev));
        bool next_ws = is_unicode_ws_byte(static_cast<unsigned char>(next));
        bool prev_punct = is_ascii_punct(prev);
        bool next_punct = is_ascii_punct(next);

        bool left_flanking = !next_ws &&
            (!next_punct || prev_ws || prev_punct);
        bool right_flanking = !prev_ws &&
            (!prev_punct || next_ws || next_punct);

        bool can_open, can_close;
        if (c == '_') {
            can_open = left_flanking && (!right_flanking || prev_punct);
            can_close = right_flanking && (!left_flanking || next_punct);
        } else {
            can_open = left_flanking;
            can_close = right_flanking;
        }

        Node n{md::Inline{md::Text{std::string(static_cast<std::size_t>(count), c)}}};
        n.is_delim = true;
        n.delim_char = c;
        n.delim_count = count;
        n.orig_count = static_cast<std::size_t>(count);
        n.can_open = can_open;
        n.can_close = can_close;
        nodes_.push_back(std::move(n));
    }

    // ── link / image resolution (§6.3, §6.4) ─────────────────────────────
    // Called when we hit ']'. Returns true if a link/image was produced
    // (advances i appropriately via member i_after_).
    std::size_t i_after_ = 0;
    bool try_close_link(std::size_t& i) {
        // find the most recent unmatched, active '[' or '!['
        int opener_idx = -1;
        for (int k = static_cast<int>(nodes_.size()) - 1; k >= 0; --k) {
            Node& n = nodes_[k];
            if (n.is_delim && (n.delim_char == '[' || n.delim_char == '!')) {
                opener_idx = k;
                break;
            }
        }
        if (opener_idx < 0) return false;
        Node& opener = nodes_[opener_idx];
        if (!opener.active) {
            opener.is_delim = false;  // deactivate → literal handled later
            opener.delim_char = 0;
            // turn into literal text
            opener.span = md::Inline{md::Text{opener.is_image ? "![" : "["}};
            return false;
        }
        bool is_image = opener.is_image;

        // Now parse what follows ']': inline (url), reference, collapsed,
        // or shortcut.
        std::size_t after = i + 1;
        std::string url, title;
        bool matched = false;
        std::vector<md::Inline> ref_kids_unused;

        // inline link/image: ( ... )
        if (after < text_.size() && text_[after] == '(') {
            std::size_t pos = after + 1;
            if (parse_inline_dest(pos, url, title)) {
                matched = true;
                after = pos;  // pos points one past ')'
            }
        }

        // reference forms
        if (!matched) {
            // [text][label]
            std::size_t pos = after;
            std::string label;
            if (pos < text_.size() && text_[pos] == '[') {
                std::size_t lp = pos + 1;
                std::string lab;
                bool ok = read_label(lp, lab);
                if (ok) {
                    if (strip(lab).empty()) {
                        // collapsed [text][] → use text as label
                        label = inline_text_between(opener_idx);
                    } else {
                        label = lab;
                    }
                    if (auto* r = lookup(label)) {
                        url = r->url; title = r->title; matched = true;
                        after = lp;
                    }
                }
            } else {
                // shortcut [text]
                label = inline_text_between(opener_idx);
                if (auto* r = lookup(label)) {
                    url = r->url; title = r->title; matched = true;
                    after = i + 1;
                }
            }
        }

        if (!matched) {
            // failed: opener becomes literal
            opener.is_delim = false;
            opener.delim_char = 0;
            opener.span = md::Inline{md::Text{is_image ? "![" : "["}};
            return false;
        }

        // Build the link/image. Inner nodes are everything after opener.
        // First process emphasis within that range.
        process_emphasis(opener_idx + 1);
        std::vector<md::Inline> kids = collect_inner(opener_idx + 1);
        std::string plain = inline_plain_text(kids);

        // remove inner nodes + opener
        nodes_.resize(static_cast<std::size_t>(opener_idx));

        if (is_image) {
            md::Image im;
            im.url = std::move(url);
            im.title = std::move(title);
            im.alt = plain;
            im.kids = std::move(kids);
            push_node(std::move(im));
        } else {
            md::Link l;
            l.url = std::move(url);
            l.title = std::move(title);
            l.text = plain;
            l.kids = std::move(kids);
            push_node(std::move(l));
            // deactivate earlier '[' openers (no links in links)
            for (auto& nn : nodes_)
                if (nn.is_delim && nn.delim_char == '[') nn.active = false;
        }

        i = after;
        return true;
    }

    // Parse inline destination starting after '(' (pos at first content).
    // On success sets url/title and advances pos past ')'.
    bool parse_inline_dest(std::size_t& pos, std::string& url, std::string& title) {
        std::string_view s = text_;
        // skip ws (incl newlines)
        auto skip = [&] {
            while (pos < s.size() && (s[pos]==' '||s[pos]=='\t'||s[pos]=='\n')) ++pos;
        };
        skip();
        url.clear(); title.clear();
        if (pos < s.size() && s[pos] == ')') { ++pos; return true; }  // empty ()
        // destination
        if (pos < s.size() && s[pos] == '<') {
            ++pos;
            std::string u;
            while (pos < s.size() && s[pos] != '>' && s[pos] != '\n') {
                if (s[pos] == '\\' && pos + 1 < s.size() && is_ascii_punct(s[pos+1])) {
                    u += s[pos+1]; pos += 2; continue;
                }
                u += s[pos++];
            }
            if (pos >= s.size() || s[pos] != '>') return false;
            ++pos;
            url = std::move(u);
        } else {
            int depth = 0;
            std::string u;
            while (pos < s.size()) {
                char c = s[pos];
                if (c == '\\' && pos + 1 < s.size() && is_ascii_punct(s[pos+1])) {
                    u += s[pos+1]; pos += 2; continue;
                }
                if (is_unicode_ws_byte(static_cast<unsigned char>(c))) break;
                if (c == '(') { ++depth; }
                else if (c == ')') { if (depth == 0) break; --depth; }
                u += c;
                ++pos;
            }
            url = std::move(u);
        }
        skip();
        // optional title
        if (pos < s.size() && (s[pos] == '"' || s[pos] == '\'' || s[pos] == '(')) {
            char open = s[pos];
            char close = open == '(' ? ')' : open;
            ++pos;
            std::string t;
            bool ok = false;
            while (pos < s.size()) {
                char c = s[pos];
                if (c == '\\' && pos + 1 < s.size() && is_ascii_punct(s[pos+1])) {
                    t += s[pos+1]; pos += 2; continue;
                }
                if (c == close) { ok = true; ++pos; break; }
                t += c; ++pos;
            }
            if (!ok) return false;
            // decode entities in title
            title = decode_text(t);
            skip();
        }
        if (pos >= s.size() || s[pos] != ')') return false;
        ++pos;
        url = decode_text(url);
        return true;
    }

    bool read_label(std::size_t& pos, std::string& out) {
        std::string_view s = text_;
        std::string lab;
        while (pos < s.size() && s[pos] != ']') {
            if (s[pos] == '[') return false;
            if (s[pos] == '\\' && pos + 1 < s.size()) { lab += s[pos]; lab += s[pos+1]; pos += 2; continue; }
            lab += s[pos++];
        }
        if (pos >= s.size() || s[pos] != ']') return false;
        ++pos;
        out = std::move(lab);
        return true;
    }

    [[nodiscard]] const md::LinkRef* lookup(std::string_view label) const {
        auto key = normalize_label(label);
        auto it = refs_.find(key);
        return it == refs_.end() ? nullptr : &it->second;
    }

    // text content between opener and current end (for shortcut labels)
    [[nodiscard]] std::string inline_text_between(int opener_idx) {
        std::string out;
        for (std::size_t k = static_cast<std::size_t>(opener_idx) + 1; k < nodes_.size(); ++k) {
            Node& n = nodes_[k];
            if (n.is_delim) {
                if (n.delim_char == '*' || n.delim_char == '_')
                    out += std::string(n.orig_count, n.delim_char);
            } else {
                out += inline_plain_text({n.span});
            }
        }
        return out;
    }

    std::vector<md::Inline> collect_inner(int from) {
        std::vector<md::Inline> out;
        for (std::size_t k = static_cast<std::size_t>(from); k < nodes_.size(); ++k) {
            Node& n = nodes_[k];
            if (n.is_delim && n.delim_count > 0) {
                out.push_back(md::Text{std::string(static_cast<std::size_t>(n.delim_count), n.delim_char)});
            } else if (n.is_delim && (n.delim_char == '[' || n.delim_char == '!')) {
                out.push_back(md::Text{n.delim_char == '!' ? "![" : "["});
            } else if (!n.is_delim) {
                out.push_back(std::move(n.span));
            }
        }
        return merge_text(std::move(out));
    }

    static std::string decode_text(std::string_view s) {
        std::string out;
        for (std::size_t i = 0; i < s.size();) {
            if (s[i] == '&') {
                std::string dec;
                std::size_t used = decode_entity(s, i, dec);
                if (used) { out += dec; i += used; continue; }
            }
            out += s[i++];
        }
        return out;
    }

    // ── emphasis processing (§6.2) ───────────────────────────────────────
    void process_emphasis(std::size_t stack_bottom) {
        // walk delimiters from stack_bottom forward looking for closers
        std::size_t i = stack_bottom;
        // openers_bottom per delim-char/length-mod
        while (i < nodes_.size()) {
            Node& closer = nodes_[i];
            if (!closer.is_delim || !closer.can_close ||
                (closer.delim_char != '*' && closer.delim_char != '_') ||
                closer.delim_count == 0) {
                ++i;
                continue;
            }
            // find matching opener walking back
            int j = static_cast<int>(i) - 1;
            int found = -1;
            while (j >= static_cast<int>(stack_bottom)) {
                Node& op = nodes_[static_cast<std::size_t>(j)];
                if (op.is_delim && op.can_open && op.delim_char == closer.delim_char &&
                    op.delim_count > 0) {
                    // rule of 3
                    bool ok = true;
                    if ((closer.can_open || op.can_close)) {
                        if ((op.orig_count + closer.orig_count) % 3 == 0 &&
                            !(op.orig_count % 3 == 0 && closer.orig_count % 3 == 0))
                            ok = false;
                    }
                    if (ok) { found = j; break; }
                }
                --j;
            }
            if (found < 0) { ++i; continue; }

            Node& op = nodes_[static_cast<std::size_t>(found)];
            int use = (op.delim_count >= 2 && closer.delim_count >= 2) ? 2 : 1;

            // Bound emphasis nesting depth. Real content never nests beyond
            // a few levels; pathological input (`**`×2000…) would build a
            // thousand-deep AST that blows the renderer's recursive layout.
            // Matches the block-nesting cap. Past the cap, delimiters stay
            // literal.
            constexpr int kMaxEmphasisDepth = 16;
            int inner_depth = 0;
            for (std::size_t k = static_cast<std::size_t>(found) + 1; k < i; ++k)
                inner_depth = std::max(inner_depth, nodes_[k].emph_depth);
            if (inner_depth >= kMaxEmphasisDepth) { ++i; continue; }

            // collect inner nodes between op and closer
            std::vector<md::Inline> inner;
            for (std::size_t k = static_cast<std::size_t>(found) + 1; k < i; ++k) {
                Node& n = nodes_[k];
                if (n.is_delim && n.delim_count > 0) {
                    inner.push_back(md::Text{std::string(static_cast<std::size_t>(n.delim_count), n.delim_char)});
                } else if (n.is_delim) {
                    if (n.delim_char == '[') inner.push_back(md::Text{"["});
                    else if (n.delim_char == '!') inner.push_back(md::Text{"!["});
                } else {
                    inner.push_back(std::move(n.span));
                }
            }
            inner = merge_text(std::move(inner));

            md::Inline node = (use == 2)
                ? md::Inline{md::Bold{std::move(inner)}}
                : md::Inline{md::Italic{std::move(inner)}};

            // consume delimiters
            op.delim_count -= use;
            closer.delim_count -= use;

            // replace the inner range [found+1, i) with the emphasis node.
            // Erase inner nodes, insert node, fix index i.
            nodes_.erase(nodes_.begin() + found + 1, nodes_.begin() + i);
            Node en{std::move(node)};
            en.emph_depth = inner_depth + 1;
            nodes_.insert(nodes_.begin() + found + 1, std::move(en));
            i = static_cast<std::size_t>(found) + 1;
            // adjust delim text representations
            // re-evaluate closer at new index
            // (closer moved to found+2 if it still has count)
            // simplest: restart from found
            if (op.delim_count == 0) {
                // opener exhausted; leave as zero-count (skipped in output)
            }
            // continue scanning from the emphasis node forward
            ++i;
        }
    }

    // merge adjacent Text nodes
    static std::vector<md::Inline> merge_text(std::vector<md::Inline> in) {
        std::vector<md::Inline> out;
        for (auto& n : in) {
            if (auto* t = std::get_if<md::Text>(&n.inner)) {
                if (!out.empty()) {
                    if (auto* p = std::get_if<md::Text>(&out.back().inner)) {
                        p->content += t->content;
                        continue;
                    }
                }
            }
            out.push_back(std::move(n));
        }
        return out;
    }
};

} // namespace

std::vector<md::Inline> parse_inlines(std::string_view text, const RefMap& refs) {
    InlineParser p(refs);
    return p.parse(text);
}

std::string inline_plain_text(const std::vector<md::Inline>& spans) {
    std::string out;
    for (const auto& s : spans) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, md::Text>) out += n.content;
            else if constexpr (std::is_same_v<T, md::Code>) out += n.content;
            else if constexpr (std::is_same_v<T, md::RawInline>) out += n.content;
            else if constexpr (std::is_same_v<T, md::HardBreak>) out += '\n';
            else if constexpr (std::is_same_v<T, md::SoftBreak>) out += ' ';
            else if constexpr (requires { n.children; })
                out += inline_plain_text(n.children);
            else if constexpr (requires { n.kids; })
                out += inline_plain_text(n.kids);
        }, s.inner);
    }
    return out;
}

} // namespace maya::md_detail::engine
