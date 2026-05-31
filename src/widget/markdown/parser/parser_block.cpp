// parser_block.cpp — the block parser (line-oriented document structure).
//
// Owns: reference-definition collection (collect_ref_defs +
// normalize_ref_label), and parse_markdown_impl — the recursive block
// scanner that recognises headings, fences, lists, tables, blockquotes,
// alerts, footnotes, definition lists, <details>, and HTML blocks — plus
// the public parse_markdown entry point.

#include "maya/widget/markdown.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "maya/widget/markdown/internal.hpp"

#include "maya/widget/markdown/parser_internal.hpp"
#include "maya/widget/markdown/engine/cm_engine.hpp"

namespace maya {

// Shared small helpers (trim / starts_with / ascii_lower / dedent /
// ol_start_num / parse_task_checkbox / find_eol) come from parser_internal.
using namespace ::maya::md_detail::parser_detail;

// Inline entry + list/table/html helpers from sibling TUs (declared in
// internal.hpp). Pulled into scope so the verbatim body calls unqualified.
using ::maya::md_detail::parse_inlines;
using ::maya::md_detail::is_table_row;
using ::maya::md_detail::is_table_separator;
using ::maya::md_detail::split_table_cells;
using ::maya::md_detail::parse_table_alignments;
using ::maya::md_detail::try_parse_html_tag;
using ::maya::md_detail::count_indent;
using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;

namespace { using RefDefsGuard = ::maya::md_detail::RefDefsScope; }

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
#if !defined(MAYA_MD_LEGACY_ENGINE)
    // New spec-faithful CommonMark core (two-phase: block structure then
    // delimiter-stack inlines). The legacy heuristic scanner below is kept
    // behind MAYA_MD_LEGACY_ENGINE and still reachable via the
    // parse_markdown_impl bridge that the streaming widget uses.
    return ::maya::md_detail::engine::parse(source);
#else
    md::Document doc;
    // Pre-pass: pull out `[label]: url "title"` definitions and remember
    // them in the Document, because inline parsing needs the map to resolve
    // references that appear *before* the definition textually.
    std::string cleaned = collect_ref_defs(source, doc.ref_defs);

    RefDefsGuard guard(&doc.ref_defs);
    auto parsed = parse_markdown_impl(cleaned, 0);
    doc.blocks = std::move(parsed.blocks);
    return doc;
#endif
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
std::size_t find_eol(const char* data, std::size_t start, std::size_t end) noexcept {
    return ::maya::md_detail::parser_detail::find_eol(data, start, end);
}
std::string collect_ref_defs(std::string_view source,
                             std::unordered_map<std::string, md::LinkRef>& defs) {
    return ::maya::collect_ref_defs(source, defs);
}
md::Document parse_markdown_impl(std::string_view source, int depth) {
    return ::maya::parse_markdown_impl(source, depth);
}
} // namespace md_detail

} // namespace maya
