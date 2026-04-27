#pragma once
// maya::widget::investigation — Recursive tree of concurrent investigations
//
// A bordered card showing a goal-driven investigation that fans out into
// concurrently-running sub-investigations and atomic actions. Models the
// "agent spawning sub-agents to explore a codebase in parallel" UX:
// each sub-investigation runs in its own subprocess (PID tagged), spinners
// for every Running node tick in lockstep off one Tick subscription.
//
// Tree branching uses ├─ │ └─ glyphs rather than nested borders — depth-N
// nested borders become unreadable past two levels. One outer border, the
// rest is text indented with tree characters.
//
//   ╭─ ● Investigate ─────────────────────────────────────────────────╮
//   │ Where is auth handled?                                  ⠋  2.3s │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈ │
//   │ ├─ ✓ Read src/main.cpp                                    0.1s │
//   │ ├─ ✓ Investigate: where is login handled?  [pid 4821]     0.8s │
//   │ │  ├─ ✓ Grep "login" in src/                              0.1s │
//   │ │  └─ ⓘ Login goes through validate_credentials()              │
//   │ ├─ ● Investigate: where is signup handled? [pid 4822]   ⠋ 1.2s │
//   │ │  └─ ● Read src/auth/signup.cpp                               │
//   │ └─ ○ Investigate: how are sessions managed?                    │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈ │
//   │ ⓘ Auth uses session tokens stored in Redis                     │
//   ╰────────────────────────────────────────────────────────────────╯
//
// API:
//   Investigation inv("Where is auth handled?");
//   inv.set_status(InvestigationStatus::Running);
//
//   auto& read = inv.add_action("Read src/main.cpp");
//   read.set_status(FindingStatus::Success);
//   read.set_elapsed(0.1f);
//
//   auto& sub = inv.add_sub("where is login handled?");
//   sub.set_pid(4821);
//   sub.set_status(InvestigationStatus::Running);
//   // ... drive sub from a worker thread ...
//
//   inv.add_note("Auth uses session tokens stored in Redis");
//   inv.advance(dt);   // ticks all spinners in the tree
//
// Stable references: findings live in std::deque, sub-investigations in
// std::unique_ptr — references returned by add_action / add_sub remain
// valid across further additions. This matters because callers typically
// hold an Action& or Investigation& on a worker thread while the main
// thread keeps appending siblings.

#include <cstdio>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "spinner.hpp"

namespace maya {

// ============================================================================
// Status enums
// ============================================================================

enum class InvestigationStatus : uint8_t { Pending, Running, Complete, Failed };
enum class FindingStatus       : uint8_t { Pending, Running, Success, Failed };

class Investigation {
public:
    // ── Action: a single atomic step (Read X, $ rg foo, Grep bar) ────────
    //
    // Held by value inside a std::deque — references returned by
    // add_action() stay valid through further add_*() calls on the parent.
    struct Action {
        std::string   label;
        std::string   detail;          // optional one-line output snippet
        FindingStatus status   = FindingStatus::Pending;
        float         elapsed  = 0.0f;
        int           pid      = 0;    // 0 = not associated with a process

        void set_label(std::string_view s)  { label  = std::string{s}; }
        void set_detail(std::string_view s) { detail = std::string{s}; }
        void set_status(FindingStatus st)   { status = st; }
        void set_elapsed(float seconds)     { elapsed = seconds; }
        void set_pid(int p)                 { pid = p; }
    };

private:
    // A finding is one of: action, sub-investigation, or inline note.
    //
    // Sub-investigations live behind unique_ptr so the Investigation itself
    // is recursively safe to move and so a returned `Investigation&` stays
    // valid even when the deque grows past its initial capacity.
    enum class FindingKind : uint8_t { ActionKind, SubKind, NoteKind };

    struct Finding {
        FindingKind                    kind;
        Action                         action;       // ActionKind
        std::unique_ptr<Investigation> sub;          // SubKind (heap-stable)
        std::string                    note;         // NoteKind
    };

    std::string         question_;
    std::string         conclusion_;
    InvestigationStatus status_   = InvestigationStatus::Pending;
    float               elapsed_  = 0.0f;
    int                 pid_      = 0;
    bool                expanded_ = true;
    std::deque<Finding> findings_;

    Spinner<SpinnerStyle::Dots> spinner_{Style{}.with_dim()};

public:
    Investigation() = default;
    explicit Investigation(std::string question) : question_(std::move(question)) {}

    // ── Configuration ─────────────────────────────────────────────────────

    void set_question(std::string_view q)   { question_   = std::string{q}; }
    void set_conclusion(std::string_view c) { conclusion_ = std::string{c}; }
    void set_status(InvestigationStatus s)  { status_     = s; }
    void set_elapsed(float seconds)         { elapsed_    = seconds; }
    void set_pid(int p)                     { pid_        = p; }
    void set_expanded(bool e)               { expanded_   = e; }
    void toggle()                           { expanded_   = !expanded_; }

    // ── Tree mutation (returned references are stable) ────────────────────

    Action& add_action(std::string label) {
        Finding f{.kind = FindingKind::ActionKind};
        f.action.label = std::move(label);
        findings_.push_back(std::move(f));
        return findings_.back().action;
    }

    Investigation& add_sub(std::string question) {
        Finding f{.kind = FindingKind::SubKind};
        f.sub = std::make_unique<Investigation>(std::move(question));
        findings_.push_back(std::move(f));
        return *findings_.back().sub;
    }

    void add_note(std::string text) {
        Finding f{.kind = FindingKind::NoteKind};
        f.note = std::move(text);
        findings_.push_back(std::move(f));
    }

    void clear() { findings_.clear(); }

    // Recursively tick spinners for this node and every nested sub.
    // Drive from a single Sub::every(...) tick rather than one timer per
    // node — keeps every spinner in the tree visually in lockstep.
    void advance(float dt) {
        spinner_.advance(dt);
        for (auto& f : findings_) {
            if (f.kind == FindingKind::SubKind && f.sub) f.sub->advance(dt);
        }
    }

    // ── Accessors ─────────────────────────────────────────────────────────

    [[nodiscard]] InvestigationStatus status() const noexcept { return status_; }
    [[nodiscard]] bool   is_expanded() const noexcept { return expanded_; }
    [[nodiscard]] size_t finding_count() const noexcept { return findings_.size(); }

    // Count of immediate children that are still running (sub or action).
    // Useful for the "[2/5 running]" badge a UI might want to show.
    [[nodiscard]] size_t running_count() const noexcept {
        size_t n = 0;
        for (auto const& f : findings_) {
            if (f.kind == FindingKind::ActionKind && f.action.status == FindingStatus::Running) ++n;
            if (f.kind == FindingKind::SubKind && f.sub
                && f.sub->status_ == InvestigationStatus::Running) ++n;
        }
        return n;
    }

    // ── Rendering ─────────────────────────────────────────────────────────

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon(status_);
        std::string border_label = " " + icon + " Investigate ";

        auto border_color = Color::bright_black();
        if (status_ == InvestigationStatus::Failed)
            border_color = Color::red();
        else if (status_ == InvestigationStatus::Running)
            border_color = Color::magenta();
        else if (status_ == InvestigationStatus::Complete)
            border_color = Color::green();

        std::vector<Element> rows;

        // Question header: question + (optional pid) + spinner-or-elapsed
        rows.push_back(build_question_row());

        if (expanded_ && !findings_.empty()) {
            rows.push_back(separator_line());
            // Render the tree with no extra indent prefix at the root.
            for (size_t i = 0; i < findings_.size(); ++i) {
                bool is_last = (i + 1 == findings_.size());
                append_finding_rows(rows, findings_[i], "", is_last);
            }
        }

        if (!conclusion_.empty()) {
            rows.push_back(separator_line());
            rows.push_back(build_conclusion_row(conclusion_));
        }

        return (dsl::v(std::move(rows))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] static IconInfo status_icon(InvestigationStatus s) noexcept {
        switch (s) {
            case InvestigationStatus::Pending:  return {"\xe2\x97\x8b", Color::bright_black()}; // ○
            case InvestigationStatus::Running:  return {"\xe2\x97\x8f", Color::magenta()};      // ●
            case InvestigationStatus::Complete: return {"\xe2\x9c\x93", Color::green()};        // ✓
            case InvestigationStatus::Failed:   return {"\xe2\x9c\x97", Color::red()};          // ✗
        }
        return {"\xe2\x97\x8b", Color::bright_black()};
    }

    [[nodiscard]] static IconInfo finding_icon(FindingStatus s) noexcept {
        switch (s) {
            case FindingStatus::Pending: return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case FindingStatus::Running: return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case FindingStatus::Success: return {"\xe2\x9c\x93", Color::green()};          // ✓
            case FindingStatus::Failed:  return {"\xe2\x9c\x97", Color::red()};            // ✗
        }
        return {"\xe2\x97\x8b", Color::bright_black()};
    }

    [[nodiscard]] static std::string format_elapsed(float seconds) {
        char buf[32];
        if (seconds < 1.0f) {
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(seconds * 1000.0f));
        } else if (seconds < 60.0f) {
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(seconds));
        } else {
            int mins = static_cast<int>(seconds) / 60;
            float secs = seconds - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(secs));
        }
        return buf;
    }

    // Dashed full-width separator. Width is allocated lazily so the line
    // always spans the actual card width even if the parent is grown by a
    // sibling on the same row.
    [[nodiscard]] static Element separator_line() {
        return Element{ComponentElement{
            .render = [](int w, int /*h*/) -> Element {
                std::string line;
                line.reserve(static_cast<size_t>(w) * 3);
                for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                return Element{TextElement{
                    .content = std::move(line),
                    .style   = Style{}.with_dim(),
                }};
            },
            .layout = {},
        }};
    }

    // The header row carrying the question text. Right-aligns either
    // a spinner glyph (Running) or the elapsed time (Complete/Failed).
    // ComponentElement so we can measure the actual width and pad the
    // gap between question and suffix to push the suffix to the edge.
    [[nodiscard]] Element build_question_row() const {
        std::string suffix;
        if (status_ == InvestigationStatus::Running) {
            suffix = std::string{spinner_.current_frame()};
            if (elapsed_ > 0.0f) {
                suffix += "  " + format_elapsed(elapsed_);
            }
        } else if (elapsed_ > 0.0f) {
            suffix = format_elapsed(elapsed_);
        }

        std::string left = question_;
        if (pid_ > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "  [pid %d]", pid_);
            left += buf;
        }

        bool is_running = (status_ == InvestigationStatus::Running);
        return Element{ComponentElement{
            .render = [left = std::move(left), suffix = std::move(suffix), is_running]
                      (int w, int /*h*/) -> Element {
                return build_padded_row(left, suffix, w, is_running);
            },
            .layout = {},
        }};
    }

    [[nodiscard]] static Element build_conclusion_row(std::string_view note) {
        // ⓘ glyph (U+24D8) + dim italic text — visually distinct from steps.
        std::string content = "\xe2\x93\x98 " + std::string{note};
        return Element{TextElement{
            .content = std::move(content),
            .style   = Style{}.with_fg(Color::cyan()).with_italic(),
            .wrap    = TextWrap::Wrap,
        }};
    }

    // Tree branch glyphs.
    static constexpr std::string_view kBranchMid    = "\xe2\x94\x9c\xe2\x94\x80 "; // ├─
    static constexpr std::string_view kBranchEnd    = "\xe2\x94\x94\xe2\x94\x80 "; // └─
    static constexpr std::string_view kBranchVert   = "\xe2\x94\x82  ";            // │
    static constexpr std::string_view kBranchSpace  = "   ";

    // Append rows for one finding into `out`. `prefix` is the accumulated
    // tree prefix from ancestors (made of kBranchVert / kBranchSpace
    // segments). `is_last` decides whether we render ├─ or └─ for this
    // finding, and what to push onto the prefix for its descendants.
    void append_finding_rows(std::vector<Element>&       out,
                             const Finding&              f,
                             std::string_view            prefix,
                             bool                        is_last) const {
        std::string branch = std::string{is_last ? kBranchEnd : kBranchMid};
        std::string child_prefix = std::string{prefix}
                                 + std::string{is_last ? kBranchSpace : kBranchVert};

        switch (f.kind) {
            case FindingKind::ActionKind: {
                out.push_back(build_action_row(f.action, std::string{prefix} + branch));
                if (!f.action.detail.empty()) {
                    out.push_back(build_detail_row(f.action.detail, child_prefix));
                }
                break;
            }
            case FindingKind::SubKind: {
                out.push_back(build_sub_header_row(*f.sub, std::string{prefix} + branch));
                // Recurse into the sub's findings using our extended prefix.
                if (f.sub->expanded_) {
                    for (size_t i = 0; i < f.sub->findings_.size(); ++i) {
                        bool sub_last = (i + 1 == f.sub->findings_.size());
                        bool has_concl_after = !f.sub->conclusion_.empty();
                        append_finding_rows(out, f.sub->findings_[i],
                                            child_prefix,
                                            sub_last && !has_concl_after);
                    }
                    if (!f.sub->conclusion_.empty()) {
                        out.push_back(build_sub_conclusion_row(f.sub->conclusion_,
                                                               child_prefix));
                    }
                }
                break;
            }
            case FindingKind::NoteKind: {
                out.push_back(build_note_row(f.note, std::string{prefix} + branch));
                break;
            }
        }
    }

    // Action: "├─ ✓ Read src/main.cpp                              0.1s"
    [[nodiscard]] static Element build_action_row(const Action& a,
                                                  std::string   prefix) {
        auto [icon, icon_color] = finding_icon(a.status);
        std::string suffix;
        if (a.elapsed > 0.0f) suffix = format_elapsed(a.elapsed);

        // Icon sits immediately after the prefix; remember its offset so we
        // can colour just the icon glyph in the rendered TextElement.
        const std::size_t icon_off  = prefix.size();
        const std::size_t icon_size = icon.size();

        std::string left = std::move(prefix);
        left += icon;
        left += " ";
        left += a.label;
        if (a.pid > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "  [pid %d]", a.pid);
            left += buf;
        }

        Style icon_style = Style{}.with_fg(icon_color);
        bool  running    = (a.status == FindingStatus::Running);

        return Element{ComponentElement{
            .render = [left = std::move(left), suffix = std::move(suffix),
                       icon_style, running, icon_off, icon_size]
                      (int w, int /*h*/) -> Element {
                return build_padded_row_styled(left, suffix, w, running,
                                               icon_off, icon_style, icon_size);
            },
            .layout = {},
        }};
    }

    // Sub header: "├─ ● Investigate: where is login handled?  [pid 4821]  0.8s"
    [[nodiscard]] static Element build_sub_header_row(const Investigation& sub,
                                                      std::string          prefix) {
        auto [icon, icon_color] = status_icon(sub.status_);
        std::string suffix;
        if (sub.status_ == InvestigationStatus::Running) {
            suffix = std::string{sub.spinner_.current_frame()};
            if (sub.elapsed_ > 0.0f) suffix += "  " + format_elapsed(sub.elapsed_);
        } else if (sub.elapsed_ > 0.0f) {
            suffix = format_elapsed(sub.elapsed_);
        }

        const std::size_t icon_off  = prefix.size();
        const std::size_t icon_size = icon.size();

        std::string left = std::move(prefix);
        left += icon;
        left += " Investigate: ";
        left += sub.question_;
        if (sub.pid_ > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "  [pid %d]", sub.pid_);
            left += buf;
        }

        Style icon_style = Style{}.with_fg(icon_color);
        bool  running    = (sub.status_ == InvestigationStatus::Running);

        return Element{ComponentElement{
            .render = [left = std::move(left), suffix = std::move(suffix),
                       icon_style, running, icon_off, icon_size]
                      (int w, int /*h*/) -> Element {
                return build_padded_row_styled(left, suffix, w, running,
                                               icon_off, icon_style, icon_size);
            },
            .layout = {},
        }};
    }

    // Detail under an action: indented one cell past the branch glyph.
    [[nodiscard]] static Element build_detail_row(std::string_view detail,
                                                  std::string      prefix) {
        std::string content = std::move(prefix);
        content += "  ";
        content += std::string{detail};
        return Element{TextElement{
            .content = std::move(content),
            .style   = Style{}.with_dim(),
            .wrap    = TextWrap::TruncateEnd,
        }};
    }

    [[nodiscard]] static Element build_note_row(std::string_view note,
                                                std::string      prefix) {
        std::string content = std::move(prefix);
        content += "\xe2\x93\x98 ";  // ⓘ
        content += std::string{note};
        return Element{TextElement{
            .content = std::move(content),
            .style   = Style{}.with_fg(Color::cyan()).with_italic(),
            .wrap    = TextWrap::Wrap,
        }};
    }

    [[nodiscard]] static Element build_sub_conclusion_row(std::string_view note,
                                                          std::string      prefix) {
        std::string content = std::move(prefix);
        content += "\xe2\x93\x98 ";  // ⓘ
        content += std::string{note};
        return Element{TextElement{
            .content = std::move(content),
            .style   = Style{}.with_fg(Color::cyan()).with_italic(),
            .wrap    = TextWrap::Wrap,
        }};
    }

    // ── Layout helpers: right-aligned suffix without flexbox ──────────────
    //
    // We do this by hand inside a single TextElement so the spinner / elapsed
    // suffix sticks to the right edge of the card without needing nested
    // h() boxes (which would each need their own grow/shrink config). One
    // TextElement also keeps wrap/truncation predictable.

    [[nodiscard]] static Element build_padded_row(std::string_view left,
                                                  std::string_view suffix,
                                                  int              w,
                                                  bool             suffix_is_spinner) {
        return build_padded_row_styled(left, suffix, w, suffix_is_spinner,
                                       /*styled_offset*/ std::string::npos,
                                       Style{}, 0);
    }

    // Variant with a single styled segment inside `left` — used to colour
    // the status icon while leaving the rest of the line in default style.
    [[nodiscard]] static Element build_padded_row_styled(
        std::string_view left,
        std::string_view suffix,
        int              w,
        bool             suffix_is_spinner,
        std::size_t      styled_offset,    // offset of styled glyph in left
        Style            styled_style,
        std::size_t      styled_len = 3 /* UTF-8 size of ●/✓/✗/○ */) {

        const int left_w   = string_width(left);
        const int suffix_w = string_width(suffix);
        int gap = w - left_w - suffix_w;
        if (gap < 1) gap = 1;

        // If the line doesn't fit at all, truncate left and drop suffix.
        if (left_w + 1 + suffix_w > w) {
            std::string truncated = detail::truncate_end(left, w);
            return Element{TextElement{
                .content = std::move(truncated),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
            }};
        }

        std::string content;
        content.reserve(left.size() + static_cast<size_t>(gap) + suffix.size());
        content += std::string{left};
        std::size_t suffix_off = content.size() + static_cast<std::size_t>(gap);
        content.append(static_cast<std::size_t>(gap), ' ');
        content += std::string{suffix};

        std::vector<StyledRun> runs;
        // Emit a base run for the unstyled left, splitting around the icon
        // if there is one.
        if (styled_offset != std::string::npos
            && styled_offset + styled_len <= left.size()) {
            if (styled_offset > 0)
                runs.push_back(StyledRun{0, styled_offset, Style{}});
            runs.push_back(StyledRun{styled_offset, styled_len, styled_style});
            std::size_t after = styled_offset + styled_len;
            if (after < left.size())
                runs.push_back(StyledRun{after, left.size() - after, Style{}});
        } else {
            runs.push_back(StyledRun{0, left.size(), Style{}});
        }
        // Gap (whitespace, no style needed but include for completeness).
        runs.push_back(StyledRun{left.size(), static_cast<std::size_t>(gap), Style{}});
        // Suffix: dim for elapsed, default colour for spinner glyph.
        if (!suffix.empty()) {
            Style sst = suffix_is_spinner ? Style{}.with_dim() : Style{}.with_dim();
            runs.push_back(StyledRun{suffix_off, suffix.size(), sst});
        }

        return Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};
    }
};

} // namespace maya
