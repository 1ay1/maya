#pragma once
// maya::widget::search_result — Search results display (Grep/Glob)
//
// Zed's search results panel + Claude Code's grep/glob output style.
//
//   ╭─ ✓ Grep ─────────────────────────────╮
//   │ "TODO" — 5 matches in 3 files  0.4s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ src/main.cpp                          │
//   │   12 │ // TODO: fix this              │
//   │   45 │ // TODO: refactor              │
//   │ src/utils.cpp                         │
//   │    8 │ // TODO: optimize              │
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

enum class SearchKind : uint8_t { Grep, Glob };
enum class SearchStatus : uint8_t { Pending, Searching, Done, Failed };

struct SearchMatch {
    int line = 0;           // 0 for glob (no line number)
    std::string content;    // matched line content or file path for glob
};

struct SearchFileGroup {
    std::string file_path;
    std::vector<SearchMatch> matches;
};

class SearchResult {
    SearchKind kind_ = SearchKind::Grep;
    std::string pattern_;
    std::vector<SearchFileGroup> groups_;
    SearchStatus status_ = SearchStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int max_matches_per_file_ = 5;

public:
    SearchResult() = default;
    explicit SearchResult(SearchKind kind, std::string pattern = "")
        : kind_(kind), pattern_(std::move(pattern)) {}

    void set_kind(SearchKind k) { kind_ = k; }
    void set_pattern(std::string_view p) { pattern_ = std::string{p}; }
    void set_status(SearchStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_matches_per_file(int n) { max_matches_per_file_ = n; }

    void clear() { groups_.clear(); }
    void add_group(SearchFileGroup group) { groups_.push_back(std::move(group)); }

    [[nodiscard]] int total_matches() const {
        int n = 0;
        for (auto const& g : groups_) n += static_cast<int>(g.matches.size());
        return n;
    }
    [[nodiscard]] int file_count() const { return static_cast<int>(groups_.size()); }
    [[nodiscard]] SearchStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        std::string tool_name = (kind_ == SearchKind::Grep) ? "Grep" : "Glob";
        std::string border_label = " " + icon + " " + tool_name + " ";

        auto border_color = Color::bright_black();
        if (status_ == SearchStatus::Failed)
            border_color = Color::red();

        std::vector<Element> rows;

        // Summary line: "pattern" — N matches in M files  0.4s
        {
            std::string content;
            std::vector<StyledRun> runs;

            auto pattern_style = Style{}.with_fg(Color::yellow());
            auto summary_style = Style{}.with_dim();

            if (!pattern_.empty()) {
                std::string quoted = "\"" + pattern_ + "\"";
                runs.push_back(StyledRun{content.size(), quoted.size(), pattern_style});
                content += quoted;
            }

            if (status_ == SearchStatus::Done || status_ == SearchStatus::Failed) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " \xe2\x80\x94 %d match%s in %d file%s",
                    total_matches(), total_matches() == 1 ? "" : "es",
                    file_count(), file_count() == 1 ? "" : "s");
                runs.push_back(StyledRun{content.size(),
                    std::string_view{buf}.size(), summary_style});
                content += buf;
            }

            if (elapsed_ > 0.0f) {
                std::string ts = "  " + format_elapsed();
                runs.push_back(StyledRun{content.size(), 2, Style{}});
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

        // Expanded: show grouped results
        if (expanded_ && !groups_.empty()) {
            // Separator
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim().with_fg(Color::bright_black()),
                    }};
                },
                .layout = {},
            }});

            auto file_style = Style{}.with_fg(Color::blue()).with_bold();
            auto lineno_style = Style{}.with_dim();
            auto pipe_style = Style{}.with_fg(Color::bright_black());
            auto match_style = Style{};

            for (auto const& group : groups_) {
                // File header
                rows.push_back(Element{TextElement{
                    .content = group.file_path,
                    .style = file_style,
                    .wrap = TextWrap::NoWrap,
                }});

                // Matches
                int shown = 0;
                for (auto const& match : group.matches) {
                    if (max_matches_per_file_ > 0 && shown >= max_matches_per_file_) {
                        int remaining = static_cast<int>(group.matches.size()) - shown;
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "    ... %d more match%s",
                            remaining, remaining == 1 ? "" : "es");
                        rows.push_back(Element{TextElement{
                            .content = buf,
                            .style = Style{}.with_dim(),
                        }});
                        break;
                    }

                    if (match.line > 0) {
                        // Grep: "  42 │ content"
                        char num_buf[8];
                        std::snprintf(num_buf, sizeof(num_buf), "%4d", match.line);
                        std::string content = std::string{"  "} + num_buf + " \xe2\x94\x82 ";
                        std::vector<StyledRun> runs;
                        runs.push_back(StyledRun{0, 2, Style{}});
                        runs.push_back(StyledRun{2, 4, lineno_style});
                        runs.push_back(StyledRun{6, 5, pipe_style});  // " │ "
                        std::size_t text_off = content.size();
                        content += match.content;
                        runs.push_back(StyledRun{text_off, match.content.size(), match_style});

                        rows.push_back(Element{TextElement{
                            .content = std::move(content),
                            .style = {},
                            .wrap = TextWrap::NoWrap,
                            .runs = std::move(runs),
                        }});
                    } else {
                        // Glob: just indent the path/content
                        rows.push_back(Element{TextElement{
                            .content = "  " + match.content,
                            .style = match_style,
                            .wrap = TextWrap::NoWrap,
                        }});
                    }
                    ++shown;
                }
            }
        }

        return (dsl::v(std::move(rows))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case SearchStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};       // ○
            case SearchStatus::Searching:
                return {"\xe2\x97\x8f", Color::yellow()};             // ●
            case SearchStatus::Done:
                return {"\xe2\x9c\x93", Color::green()};              // ✓
            case SearchStatus::Failed:
                return {"\xe2\x9c\x97", Color::red()};                // ✗
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
