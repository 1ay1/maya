// render_tail.cpp — StreamingMarkdown's opaque-tail renderer.
//
// Renders the uncommitted bytes since the last block boundary as a
// monotonic in-progress element: inside-fence literal, open-fence code
// block, ATX heading, eager list/blockquote/table for fully-terminated
// rows, and an inline-only sliding-cache paragraph fallback for the live
// line. The monotonic-height invariant lives here — see the body.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

#include "maya/widget/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::parse_inlines;
using ::maya::md_detail::flatten_inline;
using ::maya::md_detail::build_inline_row;
using ::maya::md_detail::highlight_code;
using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;
using ::maya::md_detail::count_indent;
using ::maya::md_detail::streaming::fnv1a64;

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
    // ── Eager list / blockquote rendering for completed rows ──
    //
    // Exception to the rule above: when the tail STARTS with a list
    // marker (`- `, `* `, `+ `, `\d+. `, `\d+) `) or a blockquote
    // marker (`> `), we can render each FULLY-TERMINATED row in its
    // committed shape immediately. Long lists are the worst offender
    // for the perceived "wait for the page" lag: a 12-item bullet list
    // stays in classify_blank_line's intra-blank limbo for the entire
    // duration of the stream, rendered as raw `* item` text in body
    // colour, and snaps to a styled list only when the model emits
    // its first non-list line. Same for blockquotes — `> ` markers
    // sit literal until end-of-quote.
    //
    // Monotonicity argument: each fully-terminated row has a height
    // determined by its bytes alone (text wrap inside its column).
    // The LIVE row (last line, no trailing `\n`) stays literal so
    // further bytes can extend it without snapping shape. Once a `\n`
    // lands on the live row, it joins the rendered rows above on the
    // next frame — a one-row monotonic add, identical in cost to
    // committing one extra line of an inline paragraph.
    //
    // We feed the rows through parse_markdown_impl so the rendered
    // Element is byte-identical to what commit_range would eventually
    // produce — no snap when commit finally fires. The live row falls
    // through to the inline-only path below as a separate Element
    // sitting under the rendered list.
    auto is_list_row_start = [](std::string_view line) noexcept -> bool {
        return ul_marker_len(line) > 0 || ol_marker_len(line) > 0;
    };
    auto is_quote_row_start = [](std::string_view line) noexcept -> bool {
        // Skip up to 3 leading spaces (GFM tolerance), then `> ` or `>`
        // at end-of-line.
        std::size_t k = 0;
        while (k < 4 && k < line.size() && line[k] == ' ') ++k;
        if (k >= line.size() || line[k] != '>') return false;
        // Bare `>` or `> ...` both qualify.
        return k + 1 == line.size() || line[k + 1] == ' ' || line[k + 1] == '\t';
    };

    // Probe the first line of the tail to decide whether to engage the
    // eager renderer. We deliberately don't engage on a tail whose
    // first line is plain prose followed by list rows — the prose
    // already prints inline and treating mid-buffer list rows as a
    // separate list would split the tail into two visually distinct
    // sections that the eventual commit would re-merge differently.
    auto first_line_end = body.find('\n');
    std::string_view first_line = (first_line_end == std::string_view::npos)
        ? body
        : body.substr(0, first_line_end);
    const bool tail_is_list  = is_list_row_start(first_line);
    const bool tail_is_quote = !tail_is_list && is_quote_row_start(first_line);

    if ((tail_is_list || tail_is_quote) && first_line_end != std::string_view::npos) {
        // Walk lines forward, greedily consuming everything that still
        // belongs to the same block (list rows + their continuations,
        // or `> `-prefixed rows). Stop at the first line that breaks
        // the pattern OR at the live row (the trailing partial line
        // with no `\n`).
        std::size_t cursor = 0;
        std::size_t last_committed_end = 0;   // end of last fully-terminated in-block row
        while (cursor < body.size()) {
            auto eol = body.find('\n', cursor);
            if (eol == std::string_view::npos) {
                // Live row — leave for the inline fallback.
                break;
            }
            std::string_view line = body.substr(cursor, eol - cursor);
            bool in_block;
            if (tail_is_list) {
                // A list "row" is either a new item marker OR a
                // continuation (indented or blank, where the blank's
                // intra-list semantics already kept us cohesive).
                if (is_list_row_start(line)) {
                    in_block = true;
                } else if (line.empty()) {
                    // Blank inside the list (loose-list separator).
                    // Keep going — classify_blank_line's verdict at
                    // commit time will decide whether this terminates;
                    // for rendering we just need bytes that
                    // parse_markdown_impl will recognise.
                    in_block = true;
                } else if (count_indent(line) >= 2) {
                    // Indented continuation of the previous item.
                    in_block = true;
                } else {
                    in_block = false;
                }
            } else {
                // Blockquote: lines are `> ...` or `>` (lazy
                // continuations are CommonMark legal but we conservatively
                // require the marker to keep the eager path simple).
                in_block = is_quote_row_start(line);
            }
            if (!in_block) break;
            cursor = eol + 1;
            last_committed_end = cursor;
        }

        if (last_committed_end > 0) {
            // Parse the proven-complete rows as a real markdown
            // document. parse_markdown_impl on a slice ending in `\n`
            // produces a single List or Blockquote block (plus possibly
            // a trailing empty paragraph for the loose blank, which
            // assemble_markdown's vstack handles).
            std::string_view rendered_slice = body.substr(0, last_committed_end);
            ::maya::md_detail::RefDefsScope guard(
                const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
            auto parsed = parse_markdown_impl(std::string{rendered_slice}, 0);

            std::vector<Element> kids;
            kids.reserve(parsed.blocks.size() + 1);
            for (auto& block : parsed.blocks) {
                kids.push_back(md_block_to_element(block));
            }

            // Live row (post-last-`\n` partial) renders as inline text,
            // same monotonicity rules as the paragraph fallback below.
            // We don't fold it into the parsed list because doing so
            // would change the parsed list's shape on the next frame
            // when the row terminates — the snap we're trying to
            // avoid.
            std::string_view live_row = body.substr(last_committed_end);
            // Trim leading blank lines so a separator between the
            // rendered list and the live row doesn't paint a stray
            // empty row.
            while (!live_row.empty() && live_row.front() == '\n')
                live_row.remove_prefix(1);
            if (!live_row.empty()) {
                auto live_spans = parse_inlines(live_row);
                std::string content;
                std::vector<StyledRun> runs;
                const Style base = Style{}.with_fg(colors::text);
                for (const auto& s : live_spans) {
                    flatten_inline(s, base, content, runs);
                }
                Element live_el;
                if (runs.empty()) {
                    live_el = Element{TextElement{}};
                } else if (runs.size() == 1) {
                    live_el = Element{TextElement{
                        .content = std::move(content),
                        .style   = runs[0].style,
                    }};
                } else {
                    live_el = Element{TextElement{
                        .content = std::move(content),
                        .style   = Style{}.with_fg(colors::text),
                        .runs    = std::move(runs),
                    }};
                }
                kids.push_back(std::move(live_el));
            }

            if (kids.size() == 1) return std::move(kids.front());
            return detail::vstack().gap(1)(std::move(kids)).build();
        }
        // Fell through (no fully-terminated rows yet) — the inline
        // fallback below renders the partial markers as literal text,
        // same as the historical behaviour. Once the first row gets
        // its `\n` we'll take the eager path on the next frame.
    }

    // ── Eager table rendering ─────────────────────────────────────────
    //
    // Tables are the worst offender for "wait for the page to format".
    // Without this branch the user sees raw `|` / `---` text from the
    // moment the model emits the header until either (a) a blank line
    // lands below the table (the boundary scanner commits) or (b) a
    // non-`|` line lands below (find_table_end commits). On a long
    // table that's many seconds of unstyled pipes scrolling past.
    //
    // Eager rule:
    //   * tail starts with `|` (after ≤3 spaces tolerance — GFM)
    //   * the header line is fully terminated by `\n`
    //   * the delimiter line is fully terminated AND well-formed
    //     (only `|`, `-`, `:`, space, `\r`, with at least one `-`)
    //
    // When all three hold we feed [header, delim, body-rows…] through
    // parse_markdown_impl exactly the way commit_range would, then
    // render via md_block_to_element. Any partial trailing row (no
    // closing `\n`) is held back and rendered as inline literal text
    // BELOW the formatted table, same monotonicity trick the eager
    // list path uses.
    //
    // Monotonicity: each fully-terminated body row contributes a
    // fixed number of visual rows to the rendered table (column-
    // wrapped). New `\n`-terminated rows can only ADD rows. The live
    // partial row stays inline below until its `\n` lands, at which
    // point it folds into the table on the next frame (one-row add,
    // identical Element identity for the rows already shown — the
    // table's ComponentElement keys on `data` shared_ptr identity,
    // not on this temporary fresh build, so the inner cells cache
    // misses cleanly on the new row count without ghosting).
    if (body.size() >= 1 && first_line_end != std::string_view::npos) {
        auto first_non_space_off = [](std::string_view s) -> std::size_t {
            std::size_t k = 0;
            while (k < 4 && k < s.size() && s[k] == ' ') ++k;
            return k;
        };
        std::size_t hoff = first_non_space_off(first_line);
        const bool header_pipe = hoff < first_line.size()
                                 && first_line[hoff] == '|';
        if (header_pipe) {
            // Need a fully-terminated delimiter line.
            std::size_t delim_start = first_line_end + 1;
            std::size_t delim_end = body.find('\n', delim_start);
            if (delim_end != std::string_view::npos) {
                std::string_view delim_line =
                    body.substr(delim_start, delim_end - delim_start);
                std::size_t doff = first_non_space_off(delim_line);
                bool delim_pipe = doff < delim_line.size()
                                  && delim_line[doff] == '|';
                bool delim_has_dash = false;
                bool delim_ok = delim_pipe;
                if (delim_ok) {
                    for (std::size_t k = doff;
                         k < delim_line.size(); ++k) {
                        char c = delim_line[k];
                        if (c == '-') { delim_has_dash = true; continue; }
                        if (c == '|' || c == ':' || c == ' '
                            || c == '\t' || c == '\r') continue;
                        delim_ok = false; break;
                    }
                }
                if (delim_ok && delim_has_dash) {
                    // Walk body rows: every fully-terminated `|`-prefixed
                    // line after the delimiter. Stop at the first non-
                    // `|` line OR at the live (unterminated) row.
                    std::size_t cursor = delim_end + 1;
                    std::size_t last_row_end = cursor; // end of header+delim+complete rows
                    while (cursor < body.size()) {
                        std::size_t eol = body.find('\n', cursor);
                        if (eol == std::string_view::npos) break;
                        std::string_view line =
                            body.substr(cursor, eol - cursor);
                        std::size_t off = first_non_space_off(line);
                        if (off >= line.size() || line[off] != '|')
                            break;
                        cursor = eol + 1;
                        last_row_end = cursor;
                    }

                    // Parse the proven-complete slice as a real markdown
                    // document — yields exactly one md::Table block
                    // matching what commit_range would eventually emit.
                    std::string_view table_slice =
                        body.substr(0, last_row_end);
                    ::maya::md_detail::RefDefsScope guard(
                        const_cast<std::unordered_map<
                            std::string, md::LinkRef>*>(&ref_defs_));
                    auto parsed = parse_markdown_impl(
                        std::string{table_slice}, 0);

                    std::vector<Element> kids;
                    kids.reserve(parsed.blocks.size() + 1);
                    for (auto& block : parsed.blocks) {
                        kids.push_back(md_block_to_element(block));
                    }

                    // Live trailing row (partial, no `\n`). Render as
                    // inline literal text so further bytes extend it
                    // without snapping the table's shape.
                    std::string_view live_row =
                        body.substr(last_row_end);
                    while (!live_row.empty() && live_row.front() == '\n')
                        live_row.remove_prefix(1);
                    if (!live_row.empty()) {
                        auto live_spans = parse_inlines(live_row);
                        std::string content;
                        std::vector<StyledRun> runs;
                        const Style base =
                            Style{}.with_fg(colors::text);
                        for (const auto& s : live_spans) {
                            flatten_inline(s, base, content, runs);
                        }
                        Element live_el;
                        if (runs.empty()) {
                            live_el = Element{TextElement{}};
                        } else if (runs.size() == 1) {
                            live_el = Element{TextElement{
                                .content = std::move(content),
                                .style   = runs[0].style,
                            }};
                        } else {
                            live_el = Element{TextElement{
                                .content = std::move(content),
                                .style   = Style{}.with_fg(colors::text),
                                .runs    = std::move(runs),
                            }};
                        }
                        kids.push_back(std::move(live_el));
                    }

                    if (kids.size() == 1) return std::move(kids.front());
                    return detail::vstack().gap(1)(std::move(kids)).build();
                }
            }
        }
        // Header+delim not both ready yet → fall through to inline
        // path; the literal `|` text stays visible until the next
        // frame lands more bytes and we can try the eager path again.
    }

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
    // Fast path: source_version_ matches what populated the cache —
    // bytes are guaranteed unchanged, no hash needed.  Slow path:
    // version moved, fall back to the (hash, length) key to catch the
    // case where commit_range shifted the tail base but the stable-
    // prefix slice happened to round-trip to the same bytes.
    if (tail_inline_cache_version_ != source_version_) {
        const std::uint64_t prefix_hash = fnv1a64(stable_prefix);
        if (tail_inline_cache_prefix_len_  != stable_prefix.size()
            || tail_inline_cache_prefix_hash_ != prefix_hash) {
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
            tail_inline_cache_prefix_len_  = stable_prefix.size();
            tail_inline_cache_prefix_hash_ = prefix_hash;
        }
        tail_inline_cache_version_ = source_version_;
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

} // namespace maya
