#pragma once
// maya::widget::ToolBodyPreview — compact body content for one timeline event.
//
// Sits under the AgentTimeline event's `│` connector and carries the
// glanceable "what did the tool actually do" detail per ToolKind:
//
//     Kind             use it for             rendering
//     ───────────────  ─────────────────────  ────────────────────────────
//     None             empty body             nothing
//     CodeBlock        generic head+tail      dim text with elision marker
//     Failure          generic head+tail      red text with elision marker
//     EditDiff         struct EditHunk[]      −/+ blocks with per-hunk header
//     GitDiff          unified diff text      +/-/@@ per-line coloring
//     TodoList         struct TodoItem[]      checkbox list with status icons
//     BashOutput       terminal output        dim head + emphasized failing tail
//     FileRead         file contents          line-numbered gutter + code
//     FileWrite        new file body          dim "+" prefix + lines/bytes footer
//     Json             JSON response          tokenized pretty-print w/ kv colors
//     GrepMatches      grep results           path:line:text columnar rendering
//
// Discriminated by `kind`; only the fields relevant to that kind are
// read. Returns an empty Element when there's nothing useful to show
// (Kind::None, empty text, etc.) so callers can drop it into a slot
// list unconditionally.
//
//   maya::ToolBodyPreview{{
//       .kind        = ToolBodyPreview::Kind::BashOutput,
//       .text        = stdout,
//       .failed      = exit_code != 0,
//       .exit_code   = exit_code,
//   }}.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"   // string_width, truncate_end
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class ToolBodyPreview {
public:
    enum class Kind : std::uint8_t {
        None,         // empty body
        CodeBlock,    // generic head+tail preview, dim'd in `text_color`
        Failure,      // same shape as CodeBlock but in `Color::red()`
        EditDiff,     // multi-hunk edit preview (per-hunk header + −/+ lines)
        GitDiff,      // unified diff with per-line +/-/@@ coloring
        TodoList,     // checkbox list with status icons
        BashOutput,   // terminal output: dim head, red tail when failed
        FileRead,     // code with line-number gutter
        FileWrite,    // new file body: subtle + prefix, lines/bytes footer
        Json,         // pretty-printed JSON with key/value colors
        GrepMatches,  // path:line:text matches
    };

    struct EditHunk {
        std::string old_text;
        std::string new_text;
    };

    struct TodoItem {
        enum class Status : std::uint8_t { Pending, InProgress, Completed };
        std::string content;
        Status      status = Status::Pending;
    };

    struct Config {
        Kind kind = Kind::None;

        // CodeBlock / Failure / GitDiff / BashOutput / FileRead / FileWrite /
        // Json / GrepMatches: free-text body
        std::string text;
        Color       text_color = Color::bright_white();

        // EditDiff
        std::vector<EditHunk> hunks;

        // TodoList
        std::vector<TodoItem> todos;

        // BashOutput: when failed, append `· exit N` to the last visible
        // line. We deliberately do NOT recolor the tail — the surrounding
        // timeline card already paints its border red and its connector
        // red on failure; coloring the body too would be double-flagging.
        bool failed    = false;
        int  exit_code = 0;

        // FileRead: gutter starts at this line number.
        int  start_line  = 1;

        // Suppress placeholder rendering. Default behaviour for an empty
        // body in a Running tool is to render NOTHING — the timeline's
        // own spinner conveys "in flight" already, and an additional
        // "awaiting…" row just adds column flicker as bodies pop in.
        // Set true to opt back into the placeholder.
        bool show_streaming_placeholder = false;
        bool is_streaming               = false;

        // FileWrite: append `12 lines · 482 B` footer. The byte count is
        // the reason a Write event has a body at all (the path is in the
        // header), so this stays on by default.
        bool show_footer_stats = true;

        // Tunables. Defaults are tuned for the timeline body slot, not a
        // standalone preview pane:
        //  - code_head/tail: a head+tail elision profile for free text
        //    (CodeBlock, Failure, GitDiff)
        //  - bash_tail: terminal output is tail-heavy (summaries live at
        //    the end), so BashOutput renders only the last N lines
        //  - read_head: file reads are head-heavy (the user wants to see
        //    where the file starts), so FileRead shows only the first N
        int code_head           = 4;
        int code_tail           = 3;
        int bash_tail           = 4;
        int read_head           = 5;
        int edit_head_per_side  = 6;
        int edit_tail_per_side  = 2;
        int max_edit_hunks_shown = 4;
        int max_todos_shown     = 8;
        int max_matches_shown   = 8;
    };

    explicit ToolBodyPreview(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        switch (cfg_.kind) {
            case Kind::None:        return blank();
            case Kind::CodeBlock:   return code_block(cfg_.text, cfg_.text_color);
            case Kind::Failure:     return code_block(cfg_.text, danger());
            case Kind::EditDiff:    return edit_diff();
            case Kind::GitDiff:     return git_diff();
            case Kind::TodoList:    return todo_list();
            case Kind::BashOutput:  return bash_output();
            case Kind::FileRead:    return file_read();
            case Kind::FileWrite:   return file_write();
            case Kind::Json:        return json_block();
            case Kind::GrepMatches: return grep_matches();
        }
        return blank();
    }

private:
    Config cfg_;

    static constexpr Color muted()   { return Color::bright_black(); }
    static constexpr Color success() { return Color::green(); }
    static constexpr Color danger()  { return Color::red(); }
    static constexpr Color info()    { return Color::blue(); }
    static constexpr Color accent()  { return Color::cyan(); }
    static constexpr Color number()  { return Color::yellow(); }

    // ── Line-level helpers ────────────────────────────────────────────────

    static std::vector<std::string_view> split_lines(std::string_view s) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                out.emplace_back(s.data() + start, i - start);
                start = i + 1;
            }
        }
        if (start < s.size()) out.emplace_back(s.data() + start, s.size() - start);
        return out;
    }

    struct ElidedPreview {
        std::vector<std::string_view> lines;
        int elided = 0;
        // Whether index `i` in `lines` is the position where the elision
        // marker should be inserted ABOVE (i.e., between i-1 and i).
        int elision_at = -1;
    };

    static ElidedPreview head_tail(std::string_view s, int head, int tail) {
        auto all = split_lines(s);
        const int total = static_cast<int>(all.size());
        ElidedPreview out;
        if (total <= head + tail) {
            out.lines = std::move(all);
            return out;
        }
        out.lines.reserve(static_cast<std::size_t>(head + tail));
        for (int i = 0; i < head; ++i)
            out.lines.push_back(all[static_cast<std::size_t>(i)]);
        out.elision_at = head;
        out.elided = total - head - tail;
        for (int i = total - tail; i < total; ++i)
            out.lines.push_back(all[static_cast<std::size_t>(i)]);
        return out;
    }

    static int count_lines(std::string_view s) noexcept {
        if (s.empty()) return 0;
        int n = 0;
        for (char c : s) if (c == '\n') ++n;
        // A trailing line without a newline still counts; a trailing newline
        // does not introduce a new empty line.
        if (s.back() != '\n') ++n;
        return n;
    }

    static std::string format_bytes(std::size_t n) {
        char buf[32];
        if      (n < 1024)         std::snprintf(buf, sizeof(buf), "%zu B", n);
        else if (n < 1024UL*1024)  std::snprintf(buf, sizeof(buf), "%.1f KB",
                                                 static_cast<double>(n)/1024.0);
        else                       std::snprintf(buf, sizeof(buf), "%.2f MB",
                                                 static_cast<double>(n)/(1024.0*1024.0));
        return buf;
    }

    // ── Streaming placeholder ─────────────────────────────────────────────
    //
    // Off by default — the timeline header already shows a spinner for any
    // Running tool, so an additional "awaiting…" row would just add visual
    // pop-in as bodies appear. Opt-in via `show_streaming_placeholder`.
    [[nodiscard]] Element placeholder_or_blank(std::string_view verb) const {
        if (!cfg_.show_streaming_placeholder) return dsl::blank();
        return dsl::text(std::string{verb} + "\xe2\x80\xa6",
                         Style{}.with_fg(muted()).with_italic()).build();
    }

    // ── CodeBlock: dim'd head+tail preview, single style ──────────────────
    [[nodiscard]] Element code_block(std::string_view body, Color c) const {
        using namespace dsl;
        if (body.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting output")
                                     : blank();
        }
        const auto p = head_tail(body, cfg_.code_head, cfg_.code_tail);
        return v(each_with_elision(p, fg_dim_(c))).build();
    }

    // ── BashOutput: tail-only ─────────────────────────────────────────────
    //
    // Terminal output is tail-heavy: build summaries, test counts, error
    // messages, and exit lines all live at the end. Showing the head as
    // well wastes timeline rows on `[==========]` boilerplate.  We render
    // only the last `bash_tail` lines.
    //
    // Failure is NOT recoloured here. The surrounding AgentTimeline event
    // already paints its connector and status icon red on failure; coloring
    // the body too would be double-flagging the same signal. The failed
    // path's only addition is an inline `· exit N` suffix on the final
    // line, which carries information the timeline header doesn't.
    [[nodiscard]] Element bash_output() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting output")
                                     : blank();
        }
        // head=0 means "tail only"; head_tail returns the last `bash_tail`
        // lines and reports the rest as elided.
        const auto p = head_tail(cfg_.text, /*head=*/0, cfg_.bash_tail);

        const Style line_st = fg_dim_(cfg_.text_color);
        const Style exit_st = Style{}.with_fg(danger()).with_dim();

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 1);

        const int n = static_cast<int>(p.lines.size());
        for (int i = 0; i < n; ++i) {
            if (i == p.elision_at && p.elided > 0)
                rows.push_back(elision_marker(p.elided));

            std::string_view ln = p.lines[std::size_t(i)];
            const bool last = (i + 1 == n);

            // Append "· exit N" inline to the last line on failure — keeps
            // the chrome on one row and avoids the wasted body slot.
            if (last && cfg_.failed && cfg_.exit_code != 0) {
                std::string content;
                content.reserve(ln.size() + 16);
                content += ln;

                char buf[24];
                std::snprintf(buf, sizeof(buf),
                              "  \xc2\xb7 exit %d", cfg_.exit_code);

                std::vector<StyledRun> runs;
                runs.push_back({0, ln.size(), line_st});
                std::size_t off = content.size();
                content += buf;
                runs.push_back({off, std::string{buf}.size(), exit_st});

                rows.push_back(Element{TextElement{
                    .content = std::move(content),
                    .style   = {},
                    .wrap    = TextWrap::NoWrap,
                    .runs    = std::move(runs),
                }});
            } else {
                rows.push_back(text_row(std::string{ln}, line_st));
            }
        }

        if (rows.empty()) return blank();
        return v(rows).build();
    }

    // ── FileRead: head + line gutter ──────────────────────────────────────
    //
    // File reads are head-heavy: the assistant just opened a file and the
    // user wants to confirm it's the right one. Showing 5 leading lines
    // with a line-number gutter is enough to anchor "yes, that's the file
    // I'm thinking of." If the file is bigger than the head budget we
    // append a single dim "⋯ N more" row — which is also exactly what the
    // existing elision_marker would emit, so we stay consistent.
    [[nodiscard]] Element file_read() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("reading file")
                                     : blank();
        }
        // head-only: tail=0 makes head_tail render the first read_head
        // lines and put the elision marker AFTER them.
        const auto p = head_tail(cfg_.text, cfg_.read_head, /*tail=*/0);

        const Style gutter_st = Style{}.with_fg(muted());
        const Style pipe_st   = Style{}.with_fg(muted());
        const Style code_st   = Style{}.with_fg(cfg_.text_color);

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 1);

        int line_num = cfg_.start_line;
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            std::string_view ln = p.lines[std::size_t(i)];

            // Gutter: " 42 │ "
            char numbuf[8];
            std::snprintf(numbuf, sizeof(numbuf), "%4d", line_num);
            const std::size_t numlen = std::char_traits<char>::length(numbuf);

            std::string content;
            content.reserve(numlen + 5 + ln.size());
            content += numbuf;
            content += " \xe2\x94\x82 ";   // " │ " — 1+3+1 = 5 bytes

            std::vector<StyledRun> runs;
            runs.push_back({0, numlen, gutter_st});
            runs.push_back({content.size() - 5, 5, pipe_st});
            const std::size_t code_off = content.size();
            content += ln;
            if (!ln.empty()) runs.push_back({code_off, ln.size(), code_st});

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
            ++line_num;
        }

        // Trailing elision row when there's more file below the head
        // budget. Line numbers don't continue past this point — the user
        // is told there's more, not exactly which lines were skipped.
        if (p.elided > 0) rows.push_back(elision_marker(p.elided));

        return v(rows).build();
    }

    // ── FileWrite: subtle "+" prefix + lines/bytes footer ─────────────────
    //
    // For a Write tool the BYTE COUNT is the reason the user is looking at
    // the body — the path is in the timeline header, the diff is rare to
    // need (this is a brand-new file). We keep the dim "+ " marker as a
    // diff-context hint, default-fg the actual content (no green wall),
    // and always end with a `12 lines · 482 B` footer.
    [[nodiscard]] Element file_write() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting content")
                                     : blank();
        }
        const auto p = head_tail(cfg_.text, cfg_.code_head, cfg_.code_tail);

        const Style mark_st = Style{}.with_fg(success()).with_dim();
        const Style code_st = Style{}.with_fg(cfg_.text_color);

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 2);

        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0)
                rows.push_back(elision_marker(p.elided));

            std::string_view ln = p.lines[std::size_t(i)];
            std::string content = "+ ";
            content += ln;

            std::vector<StyledRun> runs;
            runs.push_back({0, 2, mark_st});
            if (!ln.empty()) runs.push_back({2, ln.size(), code_st});

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }

        if (cfg_.show_footer_stats) {
            const int total = count_lines(cfg_.text);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d line%s \xc2\xb7 %s",
                          total, total == 1 ? "" : "s",
                          format_bytes(cfg_.text.size()).c_str());
            rows.push_back(dsl::text(std::string{buf},
                Style{}.with_fg(muted()).with_dim()).build());
        }

        return v(rows).build();
    }

    // Helper: build a styled-text row with NoWrap so AgentTimeline's per-
    // child stripe contract (1 visible row per child) holds.
    [[nodiscard]] static Element text_row(std::string content, Style st) {
        return Element{TextElement{
            .content = std::move(content),
            .style   = st,
            .wrap    = TextWrap::NoWrap,
        }};
    }

    // ── EditDiff: per-hunk header + −/+ lines with head+tail per side ─────
    [[nodiscard]] Element edit_diff() const {
        using namespace dsl;
        if (cfg_.hunks.empty()) return blank();

        const int total_hunks = static_cast<int>(cfg_.hunks.size());
        const int shown = std::min(total_hunks, cfg_.max_edit_hunks_shown);

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(shown) * 12);

        for (int i = 0; i < shown; ++i) {
            const auto& h = cfg_.hunks[static_cast<std::size_t>(i)];
            const int minus = count_lines(h.old_text);
            const int plus  = count_lines(h.new_text);

            // Per-hunk header — only when there's more than one hunk.
            if (total_hunks > 1) {
                std::string tag  = "edit " + std::to_string(i + 1) + "/"
                                 + std::to_string(total_hunks) + "  \xc2\xb7  ";
                std::string stat = "\xe2\x88\x92" + std::to_string(minus)
                                 + " / +" + std::to_string(plus);
                rows.push_back(dsl::h(
                    text(std::move(tag),  fg_dim_(muted())),
                    text(std::move(stat), fg_dim_(muted()))
                ).build());
            }

            push_diff_side(rows, h.old_text, '-', danger());
            push_diff_side(rows, h.new_text, '+', success());
        }

        if (shown < total_hunks) {
            rows.push_back(text(
                "\xe2\x8b\xaf " + std::to_string(total_hunks - shown) + " more edits",
                Style{}.with_fg(muted()).with_italic()
            ).build());
        }

        if (rows.empty()) return blank();
        return v(rows).build();
    }

    void push_diff_side(std::vector<Element>& rows, std::string_view body,
                        char marker, Color c) const {
        using namespace dsl;
        if (body.empty()) return;
        const auto p = head_tail(body, cfg_.edit_head_per_side,
                                       cfg_.edit_tail_per_side);
        const Style mark_style = Style{}.with_fg(c).with_dim();
        const Style line_style = Style{}.with_fg(c);
        const std::string mk = std::string{marker} + " ";

        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0) {
                rows.push_back(dsl::h(
                    text(mk, mark_style),
                    text("\xe2\x8b\xaf " + std::to_string(p.elided) + " more",
                         Style{}.with_fg(muted()).with_italic())
                ).build());
            }
            rows.push_back(dsl::h(
                text(mk, mark_style),
                text(std::string{p.lines[static_cast<std::size_t>(i)]}, line_style)
            ).build());
        }
    }

    // ── GitDiff: per-line coloring (+/−/@@ markers) ───────────────────────
    [[nodiscard]] Element git_diff() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting diff")
                                     : blank();
        }
        if (cfg_.text == "no changes") return blank();

        const auto p = head_tail(cfg_.text, cfg_.code_head, cfg_.code_tail);

        const Style hdr_st = fg_dim_(muted());
        const Style ctx_st = fg_dim_(cfg_.text_color);
        const Style add_st = Style{}.with_fg(success());
        const Style rem_st = Style{}.with_fg(danger());

        auto pick_style = [&](std::string_view ln) -> Style {
            if (ln.starts_with("+++") || ln.starts_with("---")
             || ln.starts_with("diff "))               return hdr_st;
            if (ln.starts_with("@@"))                  return hdr_st;
            if (!ln.empty() && ln[0] == '+')           return add_st;
            if (!ln.empty() && ln[0] == '-')           return rem_st;
            return ctx_st;
        };

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 1);
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0)
                rows.push_back(elision_marker(p.elided));
            std::string_view ln = p.lines[static_cast<std::size_t>(i)];
            rows.push_back(text(std::string{ln}, pick_style(ln)).build());
        }
        return v(rows).build();
    }

    // ── TodoList: checkbox list ───────────────────────────────────────────
    [[nodiscard]] Element todo_list() const {
        using namespace dsl;
        if (cfg_.todos.empty()) return blank();

        const int total = static_cast<int>(cfg_.todos.size());
        const int shown = std::min(total, cfg_.max_todos_shown);

        auto row = [](const TodoItem& td) {
            const char* glyph;
            Style icon_st, body_st;
            switch (td.status) {
                case TodoItem::Status::Completed:
                    glyph   = "\xe2\x9c\x93";   // ✓
                    icon_st = Style{}.with_fg(success()).with_bold();
                    body_st = fg_dim_(muted());
                    break;
                case TodoItem::Status::InProgress:
                    glyph   = "\xe2\x97\x8d";   // ◍
                    icon_st = Style{}.with_fg(info()).with_bold();
                    body_st = Style{}.with_fg(Color::bright_white());
                    break;
                case TodoItem::Status::Pending:
                default:
                    glyph   = "\xe2\x97\x8b";   // ○
                    icon_st = fg_dim_(muted());
                    body_st = fg_dim_(Color::bright_white());
                    break;
            }
            return dsl::h(
                text(std::string{glyph} + " ", icon_st),
                text(td.content, body_st)
            ).build();
        };

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(shown) + 1);
        for (int i = 0; i < shown; ++i)
            rows.push_back(row(cfg_.todos[static_cast<std::size_t>(i)]));
        if (shown < total) {
            rows.push_back(text(
                "\xe2\x8b\xaf " + std::to_string(total - shown) + " more",
                Style{}.with_fg(muted()).with_italic()
            ).build());
        }
        return v(rows).build();
    }

    // ── GrepMatches: path:line:text rows ──────────────────────────────────
    //
    // Each grep result line follows `path:linenum:text`. Splitting it out
    // lets us tint the path and line number independently of the matched
    // body — the path becomes a glanceable scan-column, the line number
    // anchors a follow-up Read, and the match itself stays in the default
    // foreground so the EYE LANDS THERE FIRST. Path/line are dim chrome,
    // the colon separators muted, the match the focal point. Lines that
    // don't fit the canonical shape (in-tree header, stray comment) just
    // render as plain text in the default fg.
    [[nodiscard]] Element grep_matches() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("searching")
                                     : blank();
        }
        const auto all = split_lines(cfg_.text);
        const int total = static_cast<int>(all.size());
        const int shown = std::min(total, cfg_.max_matches_shown);

        // Discipline: chrome dim, match default-fg. Path/line are dim
        // muted (no color) so the eye doesn't snag on them when scanning;
        // the colored part is the MATCH TEXT itself, in the default fg.
        const Style path_st = Style{}.with_fg(muted());
        const Style num_st  = Style{}.with_fg(muted());
        const Style sep_st  = Style{}.with_fg(muted());
        const Style body_st = Style{}.with_fg(cfg_.text_color);

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(shown) + 1);

        for (int i = 0; i < shown; ++i) {
            std::string_view ln = all[std::size_t(i)];

            // Find first colon. If followed by digits + colon, treat as the
            // canonical `path:line:text` shape; otherwise just emit the raw
            // line dimmed.
            const auto p1 = ln.find(':');
            if (p1 == std::string_view::npos) {
                rows.push_back(text(std::string{ln}, body_st).build());
                continue;
            }
            const auto p2 = ln.find(':', p1 + 1);
            // Validate the middle segment is digits-only.
            bool digits = (p2 != std::string_view::npos) && (p2 > p1 + 1);
            for (std::size_t j = p1 + 1; j < p2 && digits; ++j)
                if (ln[j] < '0' || ln[j] > '9') digits = false;

            if (!digits) {
                rows.push_back(text(std::string{ln}, body_st).build());
                continue;
            }

            std::string_view path  = ln.substr(0, p1);
            std::string_view linum = ln.substr(p1 + 1, p2 - p1 - 1);
            std::string_view rest  = ln.substr(p2 + 1);

            std::string content;
            content.reserve(ln.size() + 4);
            std::vector<StyledRun> runs;

            runs.push_back({content.size(), path.size(), path_st});
            content += path;
            runs.push_back({content.size(), 1, sep_st});
            content += ':';
            runs.push_back({content.size(), linum.size(), num_st});
            content += linum;
            runs.push_back({content.size(), 1, sep_st});
            content += ':';
            runs.push_back({content.size(), rest.size(), body_st});
            content += rest;

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }

        if (shown < total) {
            rows.push_back(text(
                "\xe2\x8b\xaf " + std::to_string(total - shown) + " more matches",
                Style{}.with_fg(muted()).with_italic()
            ).build());
        }
        return v(rows).build();
    }

    // ── Json: pretty-printed with key/value coloring ──────────────────────
    //
    // Single-pass tokenizer: walk the input, classify each lexeme
    // (string / number / keyword / structural) and append it to an output
    // buffer with the correct style.  Pretty-print on the fly:
    // open-brace pushes a new indent and a newline, comma stays on the
    // current line and emits a newline, close-brace pops indent.  Strings
    // that immediately precede a `:` are tagged as keys.
    //
    // If the body doesn't start with `{` or `[` after whitespace we treat
    // it as not-JSON and fall back to the dim CodeBlock — better to render
    // it readable than to colour-vomit a parser failure midway through.
    [[nodiscard]] Element json_block() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting response")
                                     : blank();
        }
        // Sniff: first non-whitespace must be `{` or `[`.
        std::size_t i0 = 0;
        while (i0 < cfg_.text.size()
               && (cfg_.text[i0] == ' ' || cfg_.text[i0] == '\n'
                || cfg_.text[i0] == '\t' || cfg_.text[i0] == '\r')) ++i0;
        if (i0 >= cfg_.text.size()
         || (cfg_.text[i0] != '{' && cfg_.text[i0] != '[')) {
            return code_block(cfg_.text, cfg_.text_color);
        }

        // Tokenize and emit per-line with run styling. We accumulate into
        // `lines`, where each line is a (string, runs) pair; at the end
        // we wrap each in a TextElement.
        struct Run { std::size_t off, len; Style st; };
        struct Line { std::string content; std::vector<StyledRun> runs; };

        // Discipline: ONE accent (cyan) for keys; everything else (values,
        // structural punctuation, keywords, numbers) shares default-fg or
        // dim. A 4-color rainbow looks like a syntax-highlighter; in a
        // 70-col timeline body the user just wants to scan for which keys
        // are present. Tinting just the keys gives the same scannability
        // without the visual chatter.
        const Style key_st    = Style{}.with_fg(accent());
        const Style str_st    = Style{}.with_fg(cfg_.text_color);
        const Style num_st    = Style{}.with_fg(cfg_.text_color);
        const Style kw_st     = Style{}.with_fg(cfg_.text_color).with_dim();
        const Style struct_st = Style{}.with_fg(muted());
        const Style err_st    = Style{}.with_fg(danger()).with_dim();

        std::vector<Line> lines;
        lines.emplace_back();
        int depth = 0;
        auto indent = [&](Line& l) {
            for (int k = 0; k < depth; ++k) l.content += "  ";
        };

        auto append = [&](std::string_view s, Style st) {
            Line& l = lines.back();
            std::size_t off = l.content.size();
            l.content += s;
            l.runs.push_back({off, s.size(), st});
        };
        auto newline = [&] {
            lines.emplace_back();
            indent(lines.back());
        };

        const std::string& src = cfg_.text;
        std::size_t i = i0;
        // Track whether the next string literal we see is a key (we just
        // emitted a `{` or a `,` and are inside an object).
        std::vector<bool> in_object;       // depth stack: true=object, false=array
        bool expect_key = false;

        while (i < src.size()) {
            char c = src[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }

            if (c == '{' || c == '[') {
                append(std::string{c}, struct_st);
                in_object.push_back(c == '{');
                expect_key = (c == '{');
                ++depth;
                ++i;
                // Peek: empty container collapses on one line.
                std::size_t j = i;
                while (j < src.size()
                       && (src[j]==' '||src[j]=='\t'||src[j]=='\n'||src[j]=='\r')) ++j;
                char close = (c == '{') ? '}' : ']';
                if (j < src.size() && src[j] == close) {
                    append(std::string{close}, struct_st);
                    --depth;
                    if (!in_object.empty()) in_object.pop_back();
                    expect_key = false;
                    i = j + 1;
                    continue;
                }
                newline();
                continue;
            }
            if (c == '}' || c == ']') {
                --depth;
                if (!in_object.empty()) in_object.pop_back();
                expect_key = false;
                newline();
                append(std::string{c}, struct_st);
                ++i;
                continue;
            }
            if (c == ',') {
                append(",", struct_st);
                ++i;
                expect_key = !in_object.empty() && in_object.back();
                newline();
                continue;
            }
            if (c == ':') {
                append(": ", struct_st);
                ++i;
                expect_key = false;
                continue;
            }
            if (c == '"') {
                // String: scan to closing quote with backslash awareness.
                std::size_t j = i + 1;
                while (j < src.size()) {
                    if (src[j] == '\\' && j + 1 < src.size()) { j += 2; continue; }
                    if (src[j] == '"') break;
                    ++j;
                }
                std::size_t end = (j < src.size()) ? j + 1 : src.size();
                std::string_view sv(src.data() + i, end - i);
                append(sv, expect_key ? key_st : str_st);
                i = end;
                expect_key = false;
                continue;
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                std::size_t j = i + 1;
                while (j < src.size()
                       && (src[j] == '.' || src[j] == 'e' || src[j] == 'E'
                        || src[j] == '+' || src[j] == '-'
                        || (src[j] >= '0' && src[j] <= '9'))) ++j;
                append(std::string_view(src.data() + i, j - i), num_st);
                i = j;
                expect_key = false;
                continue;
            }
            if (c == 't' || c == 'f' || c == 'n') {
                std::size_t j = i;
                while (j < src.size() && src[j] >= 'a' && src[j] <= 'z') ++j;
                append(std::string_view(src.data() + i, j - i), kw_st);
                i = j;
                expect_key = false;
                continue;
            }
            // Anything else is unexpected — emit as red so it's visibly
            // wrong rather than silently swallowed.
            append(std::string{c}, err_st);
            ++i;
        }

        // Render each accumulated line as a TextElement.  Trim a trailing
        // empty last line (artifact of newline-after-close).
        while (!lines.empty() && lines.back().content.empty()) lines.pop_back();

        // Apply head/tail elision at the line level.
        const int total = static_cast<int>(lines.size());
        const int head  = cfg_.code_head;
        const int tail  = cfg_.code_tail;
        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(total) + 1);

        auto push_line = [&](Line& ln) {
            rows.push_back(Element{TextElement{
                .content = std::move(ln.content),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(ln.runs),
            }});
        };
        if (total <= head + tail) {
            for (auto& ln : lines) push_line(ln);
        } else {
            for (int k = 0; k < head; ++k)
                push_line(lines[std::size_t(k)]);
            rows.push_back(elision_marker(total - head - tail));
            for (int k = total - tail; k < total; ++k)
                push_line(lines[std::size_t(k)]);
        }
        return v(rows).build();
    }

    // ── Shared helpers ────────────────────────────────────────────────────

    // Compact elision row. The previous decorative frame
    // (`· · ·  N hidden  · · ·`) competed with AgentTimeline's own stripe
    // for visual weight; a single horizontal-ellipsis glyph + count, left-
    // aligned at a 4-col indent, reads as a quiet "more here" without
    // pulling the eye.
    static Element elision_marker(int hidden) {
        // U+22EF MIDLINE HORIZONTAL ELLIPSIS — single-cell wide.
        return dsl::text("    \xe2\x8b\xaf " + std::to_string(hidden) + " more",
                         Style{}.with_fg(muted()).with_italic()).build();
    }

    // Project elided lines into a vector of styled text Elements,
    // inserting the "… N hidden" marker at the elision position.
    static std::vector<Element> each_with_elision(const ElidedPreview& p, Style st) {
        using namespace dsl;
        std::vector<Element> out;
        out.reserve(p.lines.size() + 1);
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0)
                out.push_back(elision_marker(p.elided));
            out.push_back(
                text(std::string{p.lines[static_cast<std::size_t>(i)]}, st).build());
        }
        return out;
    }

    // bright_black is already the muted tone — stacking SGR `dim` on it
    // collapses below readability on some themes; suppress in that case.
    static Style fg_dim_(Color c) {
        const bool already_muted =
            c.kind() == Color::Kind::Named
            && c.index() == static_cast<uint8_t>(AnsiColor::BrightBlack);
        return already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
