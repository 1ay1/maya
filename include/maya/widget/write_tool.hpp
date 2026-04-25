#pragma once
// maya::widget::write_tool — File creation/write card
//
// Zed's new file card + Claude Code's write operation display.
//
//   ╭─ ✓ Write ────────────────────────────╮
//   │ src/utils/helpers.ts           0.1s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ + export function debounce(         │
//   │ +   fn: Function,                   │
//   │ +   delay: number                   │
//   │ + ) { ... }                         │
//   │                     12 lines written │
//   ╰──────────────────────────────────────╯

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class WriteStatus : uint8_t { Pending, Writing, Written, Failed };

class WriteTool {
    std::string file_path_;
    std::string description_;
    std::string content_;
    WriteStatus status_ = WriteStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int max_preview_lines_ = 15;

public:
    WriteTool() = default;
    explicit WriteTool(std::string path) : file_path_(std::move(path)) {}

    void set_file_path(std::string_view p) { file_path_ = std::string{p}; }
    void set_description(std::string_view d) { description_ = std::string{d}; }
    void set_content(std::string_view c) { content_ = std::string{c}; }
    void set_status(WriteStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_preview_lines(int n) { max_preview_lines_ = n; }

    [[nodiscard]] WriteStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        // Title-bar slot: "Write" + optional one-line description so the user
        // sees the *intent* without breaking the file row underneath.
        std::string border_label = " " + icon + " Write";
        if (!description_.empty()) {
            border_label += " \xc2\xb7 ";  // middle-dot
            border_label += description_;
        }
        border_label += " ";

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == WriteStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        } else if (status_ == WriteStatus::Written) {
            border_color = Color::green();
        } else if (status_ == WriteStatus::Writing) {
            border_color = Color::yellow();
        }

        // Pre-compute byte / line stats so we can show a live counter even
        // when there's no preview yet — this is the signal the user needs to
        // tell "still streaming" from "stuck".
        const auto [bytes, lines] = content_stats();

        std::vector<Element> rows;

        // File path + elapsed
        {
            std::string content = file_path_.empty() ? std::string{"(no path)"} : file_path_;
            std::vector<StyledRun> runs;
            runs.push_back(StyledRun{0, content.size(),
                Style{}.with_fg(Color::cyan())});

            if (elapsed_ > 0.0f) {
                std::string ts = "  " + format_elapsed();
                runs.push_back(StyledRun{content.size() + 2, ts.size() - 2, Style{}.with_dim()});
                content += ts;
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Expanded: show content preview
        if (expanded_ && !content_.empty()) {
            // Separator
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim(),
                    }};
                },
                .layout = {},
            }});

            auto add_prefix_style = Style{}.with_fg(Color::green()).with_dim();
            auto code_style = Style{}.with_fg(Color::green());

            // Head + tail elision so the user always sees both the start AND
            // the end of the file. Showing only the head meant the user
            // couldn't tell if the model finished with a sensible closing
            // brace / EOF, only what it began with. With max=6 we render
            // 3 head + ellipsis + 3 tail; max=15 → 8 head + 7 tail. When
            // total_lines fits the cap (or max_preview_lines_<=0 meaning
            // "no limit"), the tail collapses into the head and we emit
            // every line exactly once.
            std::vector<std::string_view> all_lines;
            all_lines.reserve(static_cast<std::size_t>(lines) + 1);
            {
                std::string_view sv = content_;
                while (!sv.empty()) {
                    auto nl = sv.find('\n');
                    auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                    all_lines.push_back(line);
                    if (nl == std::string_view::npos) break;
                    sv = sv.substr(nl + 1);
                }
            }

            const int total = static_cast<int>(all_lines.size());
            const bool elide = max_preview_lines_ > 0 && total > max_preview_lines_;
            // Bias the head when the cap is odd (e.g. max=7 → head=4, tail=3)
            // so the start gets the extra row. The very first line of a file
            // tends to be the most identifying (shebang, package decl,
            // license header) — give it precedence over a tail row.
            const int head_n = elide ? (max_preview_lines_ + 1) / 2 : total;
            const int tail_n = elide ? max_preview_lines_ / 2       : 0;
            const int hidden = elide ? total - head_n - tail_n      : 0;

            auto emit_line = [&](std::string_view line) {
                std::string content = "+ ";
                content += line;
                std::vector<StyledRun> runs;
                runs.push_back(StyledRun{0, 2, add_prefix_style});
                if (!line.empty()) {
                    runs.push_back(StyledRun{2, line.size(), code_style});
                }
                rows.push_back(Element{TextElement{
                    .content = std::move(content),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = std::move(runs),
                }});
            };

            for (int i = 0; i < head_n && i < total; ++i) emit_line(all_lines[i]);
            if (hidden > 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf),
                              "  \xe2\x8b\xae %d hidden line%s \xe2\x8b\xae",
                              hidden, hidden == 1 ? "" : "s");
                rows.push_back(Element{TextElement{
                    .content = buf,
                    .style = Style{}.with_dim(),
                }});
            }
            for (int i = total - tail_n; i < total; ++i) emit_line(all_lines[i]);
        }

        // Live progress / outcome footer. We *always* render this for non-
        // pending states so the user can tell "still streaming" from "stuck"
        // without expanding the card. Bytes + lines update every frame as
        // the SSE buffer grows.
        rows.push_back(progress_row(bytes, lines));

        return (dsl::v(std::move(rows))
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    struct IconInfo { std::string icon; Color color; };

    // Single pass over the buffer — used by both the preview ceiling and the
    // live progress footer, so the card hits content_ at most twice per
    // frame regardless of how many rows want to know its size.
    [[nodiscard]] std::pair<std::size_t, int> content_stats() const noexcept {
        std::size_t bytes = content_.size();
        int lines = 0;
        if (bytes == 0) return {0, 0};
        for (char c : content_) if (c == '\n') ++lines;
        // Trailing line without newline still counts as a line.
        if (content_.back() != '\n') ++lines;
        return {bytes, lines};
    }

    [[nodiscard]] static std::string format_bytes(std::size_t n) {
        char buf[32];
        if (n < 1024)              std::snprintf(buf, sizeof(buf), "%zu B", n);
        else if (n < 1024 * 1024)  std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
        else                       std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
        return buf;
    }

    // Footer line. While Writing with no content yet, shows a quiet
    // "awaiting content stream…" so the user knows the request reached us
    // but the model hasn't started emitting the body. Otherwise shows a
    // verb appropriate to status + a live "L lines · B" counter.
    [[nodiscard]] Element progress_row(std::size_t bytes, int lines) const {
        const char* verb = nullptr;
        Color verb_color = Color::bright_black();
        switch (status_) {
            case WriteStatus::Pending:  verb = "queued";        break;
            case WriteStatus::Writing:  verb = bytes == 0
                                              ? "awaiting content stream\xe2\x80\xa6"
                                              : "streaming\xe2\x80\xa6";
                                        verb_color = Color::yellow();
                                        break;
            case WriteStatus::Written:  verb = "wrote";
                                        verb_color = Color::green();
                                        break;
            case WriteStatus::Failed:   verb = "failed";
                                        verb_color = Color::red();
                                        break;
        }

        std::string text;
        std::vector<StyledRun> runs;
        if (verb) {
            text = verb;
            runs.push_back(StyledRun{0, text.size(),
                Style{}.with_fg(verb_color).with_italic()});
        }

        if (bytes > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  %d line%s \xc2\xb7 %s",
                          lines, lines == 1 ? "" : "s",
                          format_bytes(bytes).c_str());
            std::size_t off = text.size();
            text += buf;
            runs.push_back(StyledRun{off, std::string{buf}.size(),
                Style{}.with_dim()});
        }

        return Element{TextElement{
            .content = std::move(text),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case WriteStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case WriteStatus::Writing:
                return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case WriteStatus::Written:
                return {"\xe2\x9c\x93", Color::green()};          // ✓
            case WriteStatus::Failed:
                return {"\xe2\x9c\x97", Color::red()};            // ✗
        }
        return {"\xe2\x97\x8b", Color::bright_black()};
    }

    [[nodiscard]] std::string format_elapsed() const {
        char buf[32];
        if (elapsed_ < 1.0f) {
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(elapsed_ * 1000.0f));
            return buf;
        }
        if (elapsed_ < 60.0f) {
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(elapsed_));
            return buf;
        }
        int mins = static_cast<int>(elapsed_) / 60;
        float secs = elapsed_ - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(secs));
        return buf;
    }
};

} // namespace maya
