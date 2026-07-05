// boundary.cpp — StreamingMarkdown's resumable block-boundary scanner.
//
// Owns three things, all driving the "where does the committed prefix end"
// decision:
//   • classify_blank_line — is a \n\n separator a real boundary or just
//     intra-list whitespace?
//   • find_table_end      — is a `| … |` run a complete GFM table yet?
//   • StreamingMarkdown::find_block_boundary — the resumable line scanner
//     that persists its cursor / fence parity across calls so each frame
//     pays O(bytes-since-last-call), not O(committed_to_EOS).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

#include "maya/widget/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;
using ::maya::md_detail::count_indent;

namespace md_detail { namespace streaming {

IntraBlank classify_blank_line(std::string_view src, std::size_t i) noexcept {
    // i is the position of the second '\n' in a \n\n pair (line-start
    // sentinel). The previous character must be '\n', otherwise this
    // wasn't called from the blank-line branch — defensive.
    if (i == 0 || src[i - 1] != '\n') return IntraBlank::No;

    // ── Look back: walk to the start of the line ending at src[i-1].
    //
    // Bounded scan: a list-marker line is at most a few dozen bytes
    // ("  123. body" plus modest body). Pathological inputs (a streamed
    // reply with hundreds of intra-list blanks, each prior "line"
    // happening to be very long) used to make this an O(n) walk per
    // blank, called O(n) times per frame from find_block_boundary —
    // O(n²) on long streamed lists. Cap at kMaxListMarkerLineLen
    // bytes: if no '\n' is found within that window the prior "line"
    // is far too long to be a list-marker line, so the IntraBlank::No
    // verdict is correct without ever inspecting the bytes.
    constexpr std::size_t kMaxListMarkerLineLen = 256;
    std::size_t prev_end = i - 1;            // exclusive of the '\n' at i-1
    std::size_t prev_start = 0;
    std::size_t scan_floor = (prev_end > kMaxListMarkerLineLen)
                                ? (prev_end - kMaxListMarkerLineLen)
                                : 0;
    bool found_start = false;
    for (std::size_t k = prev_end; k > scan_floor; --k) {
        if (src[k - 1] == '\n') { prev_start = k; found_start = true; break; }
    }
    if (!found_start && scan_floor == 0) {
        // Reached the buffer start without seeing a '\n' — prev_start
        // really is 0, treat the entire prefix as the previous line.
        prev_start = 0;
        found_start = true;
    }
    if (!found_start) {
        // Hit the look-back cap without finding a line start. The
        // prior "line" is longer than kMaxListMarkerLineLen and so
        // cannot be a list marker; the blank line therefore is not
        // an intra-list separator.
        return IntraBlank::No;
    }
    std::string_view prev_line = src.substr(prev_start, prev_end - prev_start);
    bool prev_is_ol = ol_marker_len(prev_line) > 0;
    bool prev_is_ul = ul_marker_len(prev_line) > 0;
    if (!prev_is_ol && !prev_is_ul) return IntraBlank::No;
    int prev_indent = count_indent(prev_line);

    // ── Look forward: skip any additional consecutive blanks, then read
    //                  the next line. Loose lists sometimes carry two
    //                  blank lines between items; treat the whole run
    //                  as one separator.
    std::size_t j = i;
    while (j < src.size() && src[j] == '\n') ++j;
    if (j >= src.size()) return IntraBlank::Unknown;  // next line not here yet

    std::size_t next_end = src.find('\n', j);
    bool next_complete = (next_end != std::string_view::npos);
    if (!next_complete) next_end = src.size();
    std::string_view next_line = src.substr(j, next_end - j);

    bool next_is_ol = ol_marker_len(next_line) > 0;
    bool next_is_ul = ul_marker_len(next_line) > 0;

    if (!next_is_ol && !next_is_ul) {
        // Next line has no marker. If it's still incomplete we can't
        // tell — it might become "2. ..." once a space + body arrives,
        // or it might become "Plain paragraph". Wait.
        if (!next_complete) return IntraBlank::Unknown;
        return IntraBlank::No;
    }

    // Both prev and next are list markers. Same kind + same indent
    // (±1, matching parse_markdown_impl's tolerance at line 1845) means
    // they belong to the same list.
    int next_indent = count_indent(next_line);
    bool same_kind = (prev_is_ol && next_is_ol) || (prev_is_ul && next_is_ul);
    bool same_indent = std::abs(next_indent - prev_indent) <= 1;
    if (same_kind && same_indent) return IntraBlank::Yes;
    return IntraBlank::No;
}

TableScanResult find_table_end(std::string_view src,
                               std::size_t line_start) noexcept
{
    // Skip leading spaces (up to 3 — GFM allows). A leading tab or
    // ≥4-space indent makes this a code line, not a table.
    auto first_non_space = [](std::string_view s) -> std::size_t {
        std::size_t k = 0;
        while (k < 4 && k < s.size() && s[k] == ' ') ++k;
        return k;
    };

    // Header row: if not yet terminated by '\n' we can't even check
    // that it starts with '|' meaningfully (the line is still being
    // built) — defer.
    std::size_t header_end = src.find('\n', line_start);
    if (header_end == std::string_view::npos) {
        // The caller already knows source_[line_start] == '|', so the
        // line at least PURPORTS to be a table row. Hold here.
        return {TableScan::Incomplete, 0};
    }
    auto header_line = src.substr(line_start, header_end - line_start);
    std::size_t h_off = first_non_space(header_line);
    if (h_off >= header_line.size() || header_line[h_off] != '|')
        return {TableScan::NotATable, 0};

    // Delimiter row.
    std::size_t delim_start = header_end + 1;
    if (delim_start >= src.size()) {
        // Header is terminated but delimiter hasn't started arriving.
        // Could still become a table — defer.
        return {TableScan::Incomplete, 0};
    }
    std::size_t delim_end = src.find('\n', delim_start);
    if (delim_end == std::string_view::npos) {
        // Delimiter line in progress — defer.
        return {TableScan::Incomplete, 0};
    }
    auto delim_line = src.substr(delim_start, delim_end - delim_start);
    std::size_t d_off = first_non_space(delim_line);
    if (d_off >= delim_line.size() || delim_line[d_off] != '|')
        return {TableScan::NotATable, 0};
    bool has_dash = false;
    bool ok = true;
    for (std::size_t k = d_off; k < delim_line.size(); ++k) {
        char c = delim_line[k];
        if (c == '-') { has_dash = true; continue; }
        if (c == '|' || c == ':' || c == ' ' || c == '\t' || c == '\r')
            continue;
        ok = false; break;
    }
    if (!ok || !has_dash) return {TableScan::NotATable, 0};

    // Walk body rows: each line that starts with '|' (after ≤3 spaces)
    // belongs to the table.  Stop at the first line that doesn't —
    // that line is the finality boundary.  Require it to be fully
    // terminated (have a '\n') so we don't commit speculatively on a
    // line that might yet become `| ...`.
    std::size_t pos = delim_end + 1;
    while (pos < src.size()) {
        std::size_t eol = src.find('\n', pos);
        if (eol == std::string_view::npos) {
            // Last line of the buffer isn't terminated. Can't prove
            // finality — might become a table row when more bytes
            // arrive. Defer (but the table SHAPE is proven, so this
            // is Incomplete, not NotATable).
            return {TableScan::Incomplete, 0};
        }
        auto line = src.substr(pos, eol - pos);
        std::size_t off = first_non_space(line);
        if (off >= line.size() || line[off] != '|') {
            // Finality boundary. Either a blank line (off >= size) or
            // a non-pipe line: both end the table. The table's header
            // + delimiter row are already verified above, so the
            // bytes [line_start, pos) are PROVABLY a complete table
            // and can commit now — even if N more tables (each with
            // their own trailing blanks) follow, they will each
            // commit at their own blank-line boundary instead of
            // queueing behind the first non-pipe line in the buffer.
            //
            // Previously the blank-line branch returned Incomplete
            // "so the default blank-line rule fires" — but the outer
            // scanner sees Incomplete and breaks BEFORE reaching its
            // own blank-line check, so the table (and every
            // subsequent block separated only by blank lines) sat
            // uncommitted until a non-pipe line finally landed.
            // Symptom: multiple tables stream in invisibly, then
            // burst-render together when the next paragraph arrives.
            return {TableScan::EndsAt, pos};
        }
        pos = eol + 1;
    }
    // Reached end of buffer with all lines `|`-prefixed and terminated.
    // Still incomplete: a future byte could be more table or a closing
    // line.
    return {TableScan::Incomplete, 0};
}

}} // namespace md_detail::streaming

// find_block_boundary lives on StreamingMarkdown; pull the helpers it calls
// into scope unqualified so the verbatim body below is unchanged.
using ::maya::md_detail::streaming::IntraBlank;
using ::maya::md_detail::streaming::classify_blank_line;
using ::maya::md_detail::streaming::TableScan;
using ::maya::md_detail::streaming::find_table_end;

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

    // Ledger recorder: every boundary the scanner discovers is ALSO
    // appended to scan_boundaries_ (strictly increasing, > committed_) so
    // the reveal-paced commit gate (commit_revealed_) can later commit up
    // to the largest boundary the typewriter cursor has swept — an
    // INTERMEDIATE boundary on a multi-block tail, which the single
    // last_boundary return value can't express.
    auto record = [&](size_t b) {
        last_boundary = b;
        if (b > committed_
            && (scan_boundaries_.empty() || scan_boundaries_.back() < b))
            scan_boundaries_.push_back(b);
    };

    while (i < source_.size()) {
        bool at_line_start = (i == 0 || source_[i - 1] == '\n');

        if (at_line_start) {
            bool is_code_fence = i + 3 <= source_.size() &&
                ((source_[i] == '`' && source_[i+1] == '`' && source_[i+2] == '`') ||
                 (source_[i] == '~' && source_[i+1] == '~' && source_[i+2] == '~'));
            // Math fence ($$): treat as a block fence ONLY when the
            // opener line is exactly "$$" (optionally with trailing
            // whitespace and/or \r), terminated by '\n'. Plain `$$`
            // followed by any content is paragraph-level inline math
            // (KaTeX inline) or just dollar signs in prose; treating
            // it as a block fence would bury the rest of the message
            // in an opaque fence until the next $$ surfaces. Parser
            // doesn't emit a Math block kind either, so an over-eager
            // detection here guarantees a commit-time snap.
            bool is_math_fence = false;
            if (!is_code_fence && i + 2 <= source_.size() &&
                source_[i] == '$' && source_[i+1] == '$') {
                std::size_t eol = source_.find('\n', i);
                if (eol != std::string::npos) {
                    bool only_ws = true;
                    for (std::size_t q = i + 2; q < eol; ++q) {
                        char cc = source_[q];
                        if (cc != ' ' && cc != '\t' && cc != '\r') {
                            only_ws = false; break;
                        }
                    }
                    is_math_fence = only_ws;
                }
                // If the opener line isn't terminated yet we can't
                // prove it's $$-only — leave is_math_fence false and
                // fall through to the normal line-step; a later call
                // with the '\n' present will re-evaluate.
            }
            if (is_code_fence || is_math_fence) {
                // Opening fence commits any prose that preceded it: the
                // paragraph above can be rendered immediately with its
                // final styling, even if no blank line separates them.
                if (!in_fence) record(i);
                size_t eol = source_.find('\n', i);
                if (eol == std::string::npos) {
                    // The fence line isn't newline-terminated. A CLOSING
                    // fence (in_fence) at end-of-buffer is still provably
                    // complete when the line is fence-marker-only (no
                    // info string is permitted on a closing fence): the
                    // block can't grow further within these bytes, so
                    // commit past it. This is the common Claude ending
                    // "…```" with no trailing newline.
                    //
                    // Load-bearing (verified, do NOT "simplify" away):
                    // without this eager commit the closed block sits in
                    // the tail, where render_tail's canonical path parses
                    // the still-OPEN fence (the closing ``` is the
                    // suppressed live line) and emits a phantom trailing
                    // body row — 6 rows where the committed closed block is
                    // 5. That 1-row shrink at settle is a height-
                    // monotonicity break (scratch/diag_fence: live_h=6 vs
                    // committed_h=5). Committing here makes the live render
                    // identical to the committed one.
                    //
                    // The cost is a benign, invisible metadata nit: the
                    // committed block's source_end excludes the closing
                    // fence's trailing '\n' when a chunk happens to split
                    // exactly at "...```" (streamed source_end 627 vs the
                    // one-shot 628). Proven to change ZERO rendered cells
                    // across every chunking (scratch/repro_issueA), so the
                    // trailing '\n' simply lands in the inter-block gap
                    // instead of the code block's range — no fold / lookup /
                    // render consequence.
                    //
                    // An OPENING fence (!in_fence) with no terminator stays
                    // in the tail: its language / first line are still
                    // arriving.
                    if (in_fence) {
                        // Skip the full fence-marker run (``` may be
                        // ```` etc.; ~~~ likewise) before checking the
                        // rest of the line is whitespace-only.
                        char fence_ch = source_[i];
                        std::size_t q = i;
                        while (q < source_.size() && source_[q] == fence_ch) ++q;
                        bool marker_only = true;
                        for (; q < source_.size(); ++q) {
                            char cc = source_[q];
                            if (cc != ' ' && cc != '\t' && cc != '\r') {
                                marker_only = false; break;
                            }
                        }
                        if (marker_only) {
                            in_fence = false;
                            i = source_.size();
                            record(i);
                            continue;
                        }
                    }
                    break;
                }
                in_fence = !in_fence;
                i = eol + 1;
                if (!in_fence) record(i);
                continue;
            }

            if (!in_fence) {
                // Blank line: we're at the second '\n' of a \n\n pair
                // (or source begins with '\n' at i == 0).  Default
                // behaviour: commit up to and including the second '\n';
                // the next block starts at i + 1.  Exception: intra-list
                // whitespace (loose-list blanks between two same-kind
                // list-item lines at the same indent) must NOT split
                // the list — see classify_blank_line for the rationale.
                if (source_[i] == '\n') {
                    auto verdict = classify_blank_line(
                        std::string_view{source_}, i);
                    if (verdict == IntraBlank::Yes) {
                        // Step past the blank without advancing
                        // last_boundary; the list stays cohesive in
                        // the tail until it genuinely ends.
                        i = i + 1;
                        continue;
                    }
                    if (verdict == IntraBlank::Unknown) {
                        // Next line hasn't fully arrived. Stop the
                        // scan here without consuming the blank so
                        // the next call resumes from this position
                        // when more bytes are present.
                        break;
                    }
                    record(i + 1);
                    i = i + 1;
                    continue;
                }
                // ATX heading at line start.  Commit the prose before
                // the heading; then, once the heading line itself is
                // fully terminated by '\n', advance the boundary past
                // it so the heading is rendered in its committed shape
                // immediately — without waiting for a trailing blank
                // line that may not arrive until the next paragraph
                // lands (and during long streamed answers, may take
                // seconds to do so).
                //
                // The eager-commit mirrors the table / closing-fence
                // behaviour: a structural block that's *provably*
                // complete commits at the start of the following
                // line; a still-arriving line stays in the tail where
                // render_tail's ATX special case renders it in the
                // committed style so the user sees no styling pop.
                //
                // Why this isn't extended to list / blockquote rows:
                // a list/blockquote is a single cohesive block whose
                // shape is only knowable once the block ENDS
                // (intra-list blanks vs. terminating blanks).
                // Committing each row separately would stretch a
                // tight list into singletons separated by the inter-
                // block gap — same trade we make for tables until
                // find_table_end proves them complete.
                if (source_[i] == '#') {
                    // Verify it's an ATX heading shape (1–6 '#' then
                    // a space) before treating it as a boundary —
                    // bare '#' / '#tag' at line start is a plain
                    // paragraph in CommonMark, not a heading.
                    std::size_t hashes = 0;
                    while (hashes < 6 && i + hashes < source_.size()
                           && source_[i + hashes] == '#') ++hashes;
                    bool is_atx = hashes >= 1 && hashes <= 6
                        && i + hashes < source_.size()
                        && (source_[i + hashes] == ' '
                            || source_[i + hashes] == '\t');
                    if (is_atx) {
                        record(i);
                        // If the heading line has fully arrived,
                        // anchor the boundary past it so the next
                        // paragraph/heading/etc. starts a fresh
                        // commit. If it hasn't (`eol == npos`), fall
                        // through to the line-step `break` below;
                        // the resumable scan will retry once more
                        // bytes land.
                        std::size_t heading_eol = source_.find('\n', i);
                        if (heading_eol != std::string::npos) {
                            i = heading_eol + 1;
                            record(i);
                            continue;
                        }
                    }
                }
                if (source_[i] == '|') {
                    auto r = find_table_end(
                        std::string_view{source_}, i);
                    if (r.kind == TableScan::EndsAt) {
                        // Commit the prose-before-table at the table's
                        // start (the table itself commits at r.pos
                        // — we set last_boundary there and fast-
                        // forward i past the proven table bytes).
                        record(i);
                        i = r.pos;
                        record(r.pos);
                        continue;
                    }
                    if (r.kind == TableScan::Incomplete) {
                        // Looks like a table but finality not yet
                        // proven. STOP the scan here without
                        // advancing past this line — the scanner is
                        // resumable, and we MUST re-enter this same
                        // `|` check on the next call once more bytes
                        // have landed. Advancing past the header now
                        // would leak the table into the
                        // blank-line-only commit path.
                        break;
                    }
                    // TableScan::NotATable — fall through to the
                    // normal line-step loop. The `|` is just a
                    // literal pipe in prose.
                }

                // Horizontal rule at line start. CommonMark HR: a
                // line containing ONLY repeated `*`, `_`, or `-`
                // (≥3 of the same char), with optional interior
                // spaces, optionally indented ≤3 spaces. Like ATX
                // headings, an HR is a single fully-proven block as
                // soon as the line terminates by '\n' — we can
                // commit the prose before it AND the HR itself in
                // one transition.
                //
                // Setext ambiguity: `---` (and `===`, which we never
                // treat as HR) on a line directly under a non-blank
                // text line is a setext h2 underline, not an HR.
                // The blank-line rule above commits prose at every
                // `\n\n`, so by the time we get HERE inspecting a
                // line at `i`, the previous line is EITHER part of
                // an uncommitted run (could be a setext target) OR
                // already committed (separated by `\n\n`). Only the
                // FORMER case poses setext risk.
                //
                // Conservative gate: only eager-commit `*`/`_` HRs
                // unconditionally (setext doesn't apply — it only
                // uses `=` and `-`). For `-`-style HRs, require the
                // PREVIOUS line to be empty (blank-line separator
                // already in place, so no setext attachment is
                // possible). The fall-through for `-` after text
                // preserves the existing commit-on-blank-line
                // behaviour, which correctly resolves to either HR
                // or setext at commit time via parse_markdown_impl.
                {
                    char c0 = source_[i];
                    if (c0 == '*' || c0 == '_' || c0 == '-') {
                        std::size_t hr_eol = source_.find('\n', i);
                        if (hr_eol != std::string::npos) {
                            // Inspect the line bytes (excluding the
                            // trailing '\n').
                            std::string_view ln{source_.data() + i,
                                                hr_eol - i};
                            // Allow ≤3 leading spaces (GFM).
                            std::size_t k = 0;
                            while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
                            // Must be the same marker char throughout
                            // the line, with only spaces / '\r'
                            // allowed between markers, and ≥3 marker
                            // chars total.
                            char marker = (k < ln.size()) ? ln[k] : '\0';
                            bool is_hr_shape = (marker == '*'
                                             || marker == '_'
                                             || marker == '-');
                            std::size_t markers = 0;
                            if (is_hr_shape) {
                                for (std::size_t q = k; q < ln.size(); ++q) {
                                    char cc = ln[q];
                                    if (cc == marker) { ++markers; continue; }
                                    if (cc == ' ' || cc == '\t' || cc == '\r')
                                        continue;
                                    is_hr_shape = false;
                                    break;
                                }
                            }
                            if (is_hr_shape && markers >= 3) {
                                // Setext-safety gate for `-`: only
                                // eager-commit when the previous
                                // line is blank (so this can't be
                                // an h2 underline of the line
                                // above). `*` and `_` are always
                                // safe.
                                bool safe = (marker != '-');
                                if (!safe) {
                                    // i is at a line start; previous
                                    // byte is the '\n' that ended
                                    // the prior line. The line
                                    // ABOVE is blank if walking back
                                    // past any '\r' that precedes
                                    // that '\n' lands on another
                                    // '\n' (CRLF terminator) or on
                                    // the buffer start.
                                    //
                                    // CRLF tolerance matters: pasted
                                    // markdown and some shells emit
                                    // "\r\n". Without this, a `---`
                                    // after a CRLF-terminated blank
                                    // line was classified "unsafe"
                                    // and fell through to the blank-
                                    // line commit — cosmetically
                                    // correct but the HR sat in the
                                    // tail as literal `---` until the
                                    // next paragraph landed.
                                    if (i == 0) {
                                        safe = true;
                                    } else {
                                        // Walk back past the '\n' at
                                        // i-1 and any '\r' before it.
                                        std::size_t back = i;
                                        if (back > 0 && source_[back - 1] == '\n') --back;
                                        while (back > 0 && source_[back - 1] == '\r') --back;
                                        if (back == 0) {
                                            safe = true;
                                        } else if (source_[back - 1] == '\n') {
                                            safe = true;
                                        }
                                    }
                                }
                                if (safe) {
                                    record(i);
                                    i = hr_eol + 1;
                                    record(i);
                                    continue;
                                }
                            }
                        }
                        // No '\n' yet — the line is still arriving,
                        // fall through to the normal line-step.
                        // Next call resumes here.
                    }
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

} // namespace maya
