#pragma once
// maya::widget::ToolBodyPreview — compact body content for one timeline event.
//
// Sits under the AgentTimeline event's `│` connector and carries the
// glanceable "what did the tool actually do" detail: head+tail of bash
// output, a unified-diff-style edit preview, a colored git diff, a
// truncated todo list, etc. Reusable on its own — any compact code-ish
// preview slot can use it.
//
// Discriminated by `kind`; only the fields relevant to that kind are
// read. Returns an empty Element when there's nothing useful to show
// (kind=None, empty text, etc.) so callers can drop it into a slot
// list unconditionally.
//
//   maya::ToolBodyPreview{{
//       .kind  = ToolBodyPreview::Kind::EditDiff,
//       .hunks = { {.old_text = "...", .new_text = "..."} },
//   }}.build();
//
//   maya::ToolBodyPreview{{
//       .kind = ToolBodyPreview::Kind::CodeBlock,
//       .text = bash_output,
//   }}.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
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

        // CodeBlock / Failure / GitDiff: free-text body
        std::string text;
        Color       text_color = Color::bright_white();

        // EditDiff
        std::vector<EditHunk> hunks;

        // TodoList
        std::vector<TodoItem> todos;

        // Tunables — sensible defaults bake in moha's previous numbers.
        int code_head = 4;
        int code_tail = 3;
        int edit_head_per_side = 6;
        int edit_tail_per_side = 2;
        int max_edit_hunks_shown = 4;
        int max_todos_shown = 8;
    };

    explicit ToolBodyPreview(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        switch (cfg_.kind) {
            case Kind::None:      return blank();
            case Kind::CodeBlock: return code_block(cfg_.text, cfg_.text_color);
            case Kind::Failure:   return code_block(cfg_.text, danger());
            case Kind::EditDiff:  return edit_diff();
            case Kind::GitDiff:   return git_diff();
            case Kind::TodoList:  return todo_list();
        }
        return blank();
    }

private:
    Config cfg_;

    static constexpr Color muted() { return Color::bright_black(); }
    static constexpr Color success() { return Color::green(); }
    static constexpr Color danger()  { return Color::red(); }
    static constexpr Color info()    { return Color::blue(); }

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

    static int count_lines(std::string_view s) {
        if (s.empty()) return 0;
        int n = 1;
        for (char c : s) if (c == '\n') ++n;
        if (s.back() == '\n') --n;
        return std::max(0, n);
    }

    // ── CodeBlock: dim'd head+tail preview, single style ──────────────────
    [[nodiscard]] Element code_block(std::string_view body, Color c) const {
        using namespace dsl;
        if (body.empty()) return blank();
        const auto p = head_tail(body, cfg_.code_head, cfg_.code_tail);
        return v(each_with_elision(p, fg_dim_(c))).build();
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
                "\xe2\x80\xa6 " + std::to_string(total_hunks - shown) + " more edits",
                fg_dim_(muted())
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
                    text("\xe2\x80\xa6 " + std::to_string(p.elided) + " hidden",
                         fg_dim_(muted()))
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
        if (cfg_.text.empty() || cfg_.text == "no changes") return blank();

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
                "\xe2\x80\xa6 " + std::to_string(total - shown) + " more",
                fg_dim_(muted())
            ).build());
        }
        return v(rows).build();
    }

    // ── Shared helpers ────────────────────────────────────────────────────

    static Element elision_marker(int hidden) {
        return dsl::text("\xc2\xb7 \xc2\xb7 \xc2\xb7  "
                         + std::to_string(hidden) + " hidden  \xc2\xb7 \xc2\xb7 \xc2\xb7",
                         fg_dim_(muted())).build();
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
