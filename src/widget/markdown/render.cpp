// render.cpp — Markdown AST → maya Element conversion.
//
// Owns: namespace colors (terminal-adaptive palette), flatten_inline
// (the hot per-frame inline path), md_inline_to_element /
// build_inline_row / measure_inline_width, render_list (depth-aware),
// and md_block_to_element (incl. inline table layout machinery).
//
// Calls highlight_code via maya::md_detail (syntax.cpp owns it).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {

// Pull highlight_code into namespace maya so md_block_to_element can
// call it unqualified (it lives in maya::md_detail).
using ::maya::md_detail::highlight_code;

// colors:: lives in internal.hpp now, so render.cpp's body just sees
// it via the existing include above.

namespace {

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

} // anonymous

// md_inline_to_element is public (declared in markdown.hpp).
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

// Forward decl: render_list and md_block_to_element call each other
// recursively. md_block_to_element is the public one (declared in
// markdown.hpp); render_list is anon-ns, so the fwd decl needs to be
// outside the anon namespace below.
Element md_block_to_element(const md::Block& block);

namespace {
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

} // anonymous

// md_block_to_element is public (declared in markdown.hpp).
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

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
void flatten_inline(const md::Inline& span,
                    const Style& inherited,
                    std::string& out,
                    std::vector<StyledRun>& runs) {
    ::maya::flatten_inline(span, inherited, out, runs);
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
