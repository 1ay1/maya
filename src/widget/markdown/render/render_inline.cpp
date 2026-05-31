// render_inline.cpp — inline AST → Element (the hot per-frame path).
//
// flatten_inline collapses inline spans into one TextElement with styled
// runs so word-wrap operates on the full flow; md_inline_to_element wraps a
// single span; build_inline_row / measure_inline_width / render_list build
// the paragraph and list scaffolding. The committed-block renderer
// (render_block.cpp) calls into these via maya::md_detail.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/html.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {

// md_block_to_element lives in render_block.cpp; declared here so render_list
// (which recurses into nested block children) can call it.
Element md_block_to_element(const md::Block& block);

namespace {

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
            // Inline code: bright_cyan fg on a faint bg — a chip, not
            // just colored text. Pure-color was indistinguishable from
            // a regularly-styled word on themes where bright_cyan
            // collapses toward the body fg, so identifiers like
            // `Domain` / `StreamState` blended into prose. The bg fills
            // strictly the code RUN (no padding spaces) so wrapping
            // behaves identically to plain text — the historical
            // "chip with spaces" wrap bug doesn't reappear. On terminals
            // without true-color the ANSI black slot still produces a
            // visible step against default bg.
            auto sty = Style{}.with_fg(colors::code_fg)
                              .with_bg(colors::code_bg);
            runs.push_back({out.size(), c.content.size(), sty});
            out += c.content;
        },
        [&](const md::Link& l) {
            auto sty = Style{}.with_fg(colors::link_fg).with_underline();
            runs.push_back({out.size(), l.text.size(), sty});
            out += l.text;
        },
        [&](const md::Image& img) {
            // Pick the visible label:
            //   1. The author-provided alt text, when present.
            //   2. The URL's last path segment ("icon.png" out of
            //      `https://x/a/b/icon.png`), so `![](url)` shows
            //      something more useful than a lonely 🖼 glyph.
            //   3. The URL title, when even the URL is empty (e.g.
            //      reference image whose ref-def carries a title).
            //   4. "image" — absolute last resort, so layout never
            //      collapses to zero-width.
            std::string label = img.alt;
            if (label.empty()) {
                std::string_view url = img.url;
                // Strip query / fragment first so they don't leak into
                // the label when the URL has no trailing filename
                // (`https://x/?foo=1`).
                auto q = url.find_first_of("?#");
                if (q != std::string_view::npos) url = url.substr(0, q);
                auto slash = url.find_last_of('/');
                std::string_view tail =
                    (slash == std::string_view::npos) ? url
                                                      : url.substr(slash + 1);
                if (!tail.empty()) label.assign(tail);
            }
            if (label.empty()) label = img.title;
            if (label.empty()) label = "image";

            std::string display = "\xf0\x9f\x96\xbc " + label;
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
            // Render <kbd>X</kbd> as a key-cap chip:
            //
            //   ╶ X ╴
            //
            // The bracket glyphs (U+2576 ╶ "box drawings light right" /
            // U+2574 ╴ "box drawings light left") read as a chip
            // outline at terminal resolution without the legibility
            // hit of `[`/`]` (which collide with reference-link
            // bracketed text in scanned prose). The inner text is
            // bold + bright; the chrome rides in `kbd_border` and is
            // dimmed so the chip outline is visible without
            // out-competing the key glyphs themselves.
            auto bracket_sty =
                Style{}.with_fg(colors::kbd_border).with_dim();
            auto inner_sty = fold_style(inherited,
                Style{}.with_bold().with_fg(colors::kbd_fg));
            static constexpr std::string_view kOpen  = "\xe2\x95\xb6 "; // ╶ + space
            static constexpr std::string_view kClose = " \xe2\x95\xb4"; // space + ╴
            runs.push_back({out.size(), kOpen.size(), bracket_sty});
            out.append(kOpen);
            for (auto& child : k.children) flatten_inline(child, inner_sty, out, runs);
            runs.push_back({out.size(), kClose.size(), bracket_sty});
            out.append(kClose);
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
        [&](const md::SoftBreak&) {
            // A soft break renders as a single space in flowed terminal
            // text (the layout engine re-wraps); the spec maps it to a
            // newline in HTML but for the TUI a space is the right join.
            runs.push_back({out.size(), 1, inherited});
            out += ' ';
        },
        [&](const md::RawInline& r) {
            // Verbatim inline HTML / passthrough — show the literal text
            // dimmed so it's visibly "raw" without dominating prose.
            auto sty = fold_style(inherited, Style{}.with_fg(colors::footnote_fg));
            runs.push_back({out.size(), r.content.size(), sty});
            out += r.content;
        },
    }, span.inner);
}

// Map an HTML phrasing role onto the markdown palette so inline raw HTML
// (`<b>`, `<code>`, `<kbd>`, …) looks identical to the equivalent markdown
// construct. Mirrors the per-variant styling in flatten_inline above.
[[nodiscard]] static Style html_role_style(html::Role r, Style cur) {
    using R = html::Role;
    switch (r) {
        case R::Bold:      return cur.with_bold().with_fg(colors::bold_fg);
        case R::Italic:    return cur.with_italic().with_fg(colors::italic_fg);
        case R::Underline: return cur.with_underline();
        case R::Strike:    return cur.with_strikethrough().with_fg(colors::strike_fg);
        case R::Code:      return cur.with_fg(colors::code_fg).with_bg(colors::code_bg);
        case R::KeyCap:    return cur.with_bold().with_fg(colors::kbd_fg);
        case R::Mark:      return cur.with_bg(colors::highlight_bg)
                                     .with_fg(colors::highlight_fg);
        case R::Small:     return cur.with_dim();
        case R::Sub:
        case R::Sup:       return cur.with_fg(colors::strike_fg);
        case R::Link:      return cur.with_fg(colors::link_fg).with_underline();
        case R::None:
        case R::Break:     break;
    }
    return cur;
}

// Flatten a run of inline spans, interpreting interleaved raw-HTML tags
// (md::RawInline) as a style stack: `<b>`…`</b>` toggles bold across the
// sibling spans between them, `<br>` is a hard break, unknown tags fall back
// to the dimmed-literal passthrough. This is the markdown widget delegating
// HTML *semantics* to maya::html while keeping ownership of the span walk.
static void flatten_inlines(const std::vector<md::Inline>& spans,
                            const Style& base, std::string& out,
                            std::vector<StyledRun>& runs) {
    struct Open { std::string name; Style prev; };
    std::vector<Open> stack;
    Style cur = base;
    for (const auto& span : spans) {
        const auto* raw = std::get_if<md::RawInline>(&span.inner);
        if (!raw) { flatten_inline(span, cur, out, runs); continue; }

        auto tag = html::parse_tag(raw->content);
        if (tag) {
            html::Role role = html::inline_role(tag->name);
            if (role == html::Role::Break) {
                runs.push_back({out.size(), 1, cur});
                out += '\n';
                continue;
            }
            if (tag->name == "wbr") continue;  // zero-width break opportunity
            if (role != html::Role::None) {
                if (tag->is_close) {
                    for (int k = static_cast<int>(stack.size()) - 1; k >= 0; --k)
                        if (stack[static_cast<std::size_t>(k)].name == tag->name) {
                            cur = stack[static_cast<std::size_t>(k)].prev;
                            stack.erase(stack.begin() + k, stack.end());
                            break;
                        }
                } else if (!tag->self_closing) {
                    stack.push_back({tag->name, cur});
                    cur = html_role_style(role, cur);
                }
                continue;  // styling tags emit no literal text
            }
        }
        // not a recognized tag → dimmed-literal passthrough (flatten_inline's
        // RawInline branch), using the current effective style.
        flatten_inline(span, cur, out, runs);
    }
}

} // anonymous

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
    flatten_inlines(spans, base, content, runs);
    return string_width(content);
}

namespace {
static Element build_inline_row(const std::vector<md::Inline>& spans) {
    if (spans.empty()) return Element{TextElement{}};

    std::string content;
    std::vector<StyledRun> runs;
    Style base = Style{}.with_fg(colors::text);

    flatten_inlines(spans, base, content, runs);

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
            // ▸ (U+25B8, black right-pointing small triangle) reads as
            // a directional "item start" cue — heavier than • (which
            // disappears against dense prose on most themes) and
            // unambiguously punctuation, not stray text. Bold + bullet
            // color so it's the visual anchor for each list item.
            prefix = "  \xe2\x96\xb8 ";  // "  ▸ "
            prefix_style = Style{}.with_fg(colors::list_bullet).with_bold();
        } else {
            // Nested items step down in visual weight: hollow ◦
            // signals "sub-item" without competing with the parent
            // ▸. Extra two-space indent reinforces the hierarchy.
            prefix = "    \xe2\x97\xa6 ";  // "    ◦ "
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
        flatten_inlines(item.spans, base, body, body_runs);
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

} // anonymous

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
void flatten_inline(const md::Inline& span,
                    const Style& inherited,
                    std::string& out,
                    std::vector<StyledRun>& runs) {
    ::maya::flatten_inline(span, inherited, out, runs);
}
void flatten_inlines(const std::vector<md::Inline>& spans,
                     const Style& base,
                     std::string& out,
                     std::vector<StyledRun>& runs) {
    ::maya::flatten_inlines(spans, base, out, runs);
}
Element build_inline_row(const std::vector<md::Inline>& spans) {
    return ::maya::build_inline_row(spans);
}
int measure_inline_width(const std::vector<md::Inline>& spans) {
    return ::maya::measure_inline_width(spans);
}
Element render_list(const md::List& l, int depth) {
    return ::maya::render_list(l, depth);
}
} // namespace md_detail

} // namespace maya
