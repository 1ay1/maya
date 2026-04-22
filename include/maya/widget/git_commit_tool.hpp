#pragma once
// maya::widget::git_commit_tool — git commit operation card
//
// Matches Write/Edit/Bash card conventions so git_commit sits cleanly in the
// thread instead of falling back to the generic ToolCall card.
//
//   ╭─ ✓ Commit ───────────────────────────╮
//   │ Fix null-deref in auth.cpp     0.4s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ [master abc1234]  2 files, +12 / -3  │
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

enum class GitCommitStatus : uint8_t { Pending, Running, Done, Failed };

class GitCommitTool {
    std::string message_;
    std::string output_;
    GitCommitStatus status_ = GitCommitStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;

public:
    GitCommitTool() = default;
    explicit GitCommitTool(std::string message) : message_(std::move(message)) {}

    void set_message(std::string_view m) { message_ = std::string{m}; }
    void set_output(std::string_view o) { output_ = std::string{o}; }
    void set_status(GitCommitStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }

    [[nodiscard]] GitCommitStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, _] = status_icon();
        std::string border_label = " " + icon + " Commit ";

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == GitCommitStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        } else if (status_ == GitCommitStatus::Done) {
            border_color = Color::green();
        }

        std::vector<Element> rows;

        // Headline: the first line of the commit message (bold) + elapsed.
        {
            std::string headline = first_line(message_);
            std::string content = headline;
            std::vector<StyledRun> runs;
            if (!headline.empty())
                runs.push_back(StyledRun{0, headline.size(), Style{}.with_bold()});
            if (elapsed_ > 0.0f) {
                std::string ts = content.empty() ? format_elapsed()
                                                 : std::string{"  "} + format_elapsed();
                std::size_t off = content.size();
                runs.push_back(StyledRun{off, ts.size(), Style{}.with_dim()});
                content += ts;
            }
            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Summary row: the `[branch hash]` + insertion/deletion counts parsed
        // from the git commit output. Hidden while pending.
        if (status_ == GitCommitStatus::Done || status_ == GitCommitStatus::Failed) {
            auto summary = summarize(output_);
            if (!summary.empty()) {
                rows.push_back(Element{TextElement{
                    .content = std::move(summary),
                    .style = Style{}.with_dim(),
                    .wrap = TextWrap::NoWrap,
                }});
            }
        }

        if (expanded_ && !output_.empty()) {
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim(),
                    }};
                },
                .layout = {},
            }});
            std::string_view sv = output_;
            while (!sv.empty()) {
                auto nl = sv.find('\n');
                auto line = nl == std::string_view::npos ? sv : sv.substr(0, nl);
                sv = nl == std::string_view::npos ? std::string_view{} : sv.substr(nl + 1);
                rows.push_back(Element{TextElement{
                    .content = std::string{line},
                    .style = Style{},
                    .wrap = TextWrap::Wrap,
                }});
            }
        }

        return (dsl::v(std::move(rows))
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    [[nodiscard]] static std::string first_line(std::string_view s) {
        auto nl = s.find('\n');
        return std::string{nl == std::string_view::npos ? s : s.substr(0, nl)};
    }

    // Parse "[branch hash] subject\n N files changed, A insertions(+), D deletions(-)"
    // and render a compact single-line summary.  Gracefully degrades when the
    // output is anything else (conflict, nothing-to-commit, …) by returning
    // the first non-empty line.
    [[nodiscard]] static std::string summarize(std::string_view out) {
        if (out.empty()) return {};
        std::string_view first;
        {
            auto nl = out.find('\n');
            first = nl == std::string_view::npos ? out : out.substr(0, nl);
        }
        std::string_view stats;
        {
            auto pos = out.find("file");
            if (pos != std::string_view::npos) {
                auto ls = out.rfind('\n', pos);
                auto le = out.find('\n', pos);
                auto begin = ls == std::string_view::npos ? 0 : ls + 1;
                auto end   = le == std::string_view::npos ? out.size() : le;
                stats = out.substr(begin, end - begin);
                while (!stats.empty() && stats.front() == ' ')
                    stats.remove_prefix(1);
            }
        }
        // [branch hash] bit
        std::string head;
        if (!first.empty() && first.front() == '[') {
            auto close = first.find(']');
            if (close != std::string_view::npos)
                head = std::string{first.substr(0, close + 1)};
        }
        if (head.empty() && stats.empty()) return std::string{first};
        std::string out_s;
        if (!head.empty()) out_s += head;
        if (!stats.empty()) {
            if (!out_s.empty()) out_s += "  ";
            out_s += std::string{stats};
        }
        return out_s;
    }

    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case GitCommitStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case GitCommitStatus::Running:
                return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case GitCommitStatus::Done:
                return {"\xe2\x9c\x93", Color::green()};          // ✓
            case GitCommitStatus::Failed:
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
