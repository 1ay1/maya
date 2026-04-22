#pragma once
// maya::widget::bash_tool — Command execution card
//
// Zed's bordered terminal card + Claude Code's command status UX.
//
//   ╭─ ● Execute ──────────────────────────╮
//   │ $ npm install --save-dev typescript   │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ added 1 package in 2.3s              │
//   ╰──────────────────────────────────────╯
//
// Collapsed:
//   ╭─ ✓ Execute ──────────────────────────╮
//   │ $ npm install --save-dev typescript   │
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

enum class BashStatus : uint8_t { Pending, Running, Success, Failed };

class BashTool {
    std::string command_;
    std::string output_;
    BashStatus status_ = BashStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int exit_code_ = 0;
    int max_output_lines_ = 0;  // 0 = unlimited

public:
    BashTool() = default;
    explicit BashTool(std::string command) : command_(std::move(command)) {}

    void set_command(std::string_view cmd) { command_ = std::string{cmd}; }
    void set_output(std::string_view out) { output_ = std::string{out}; }
    void append_output(std::string_view text) { output_ += text; }
    void set_status(BashStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_exit_code(int code) { exit_code_ = code; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_output_lines(int n) { max_output_lines_ = n; }

    [[nodiscard]] BashStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Status icon for border label
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Execute ";

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == BashStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        }

        std::vector<Element> rows;

        // Command line: "$ command                    1.6s"
        //
        // Wrapped in a ComponentElement so we can measure the actual card
        // width at paint time and truncate the command when a long invocation
        // would push the elapsed suffix off-screen. Plain NoWrap text clips
        // blindly from the right — which hides precisely the bit
        // (elapsed time / status) the user needs most during execution.
        {
            std::string cmd = command_;
            std::string suffix = elapsed_ > 0.0f ? ("  " + format_elapsed()) : std::string{};
            rows.push_back(Element{ComponentElement{
                .render = [cmd = std::move(cmd), suffix](int w, int /*h*/) -> Element {
                    return build_command_row(cmd, suffix, w);
                },
                .layout = {},
            }});
        }

        // Exit code badge if failed
        if (status_ == BashStatus::Failed && exit_code_ != 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "exit code %d", exit_code_);
            rows.push_back(Element{TextElement{
                .content = buf,
                .style = Style{}.with_fg(Color::red()).with_dim(),
            }});
        }

        // Output (expanded only)
        if (expanded_ && !output_.empty()) {
            // Dashed separator
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

            // Output lines — terminal default fg
            auto output_style = Style{};
            std::string_view sv = output_;
            int line_count = 0;

            while (!sv.empty()) {
                auto nl = sv.find('\n');
                auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                if (max_output_lines_ > 0 && line_count >= max_output_lines_) {
                    char buf[32];
                    int remaining = 0;
                    auto tmp = sv;
                    while (!tmp.empty()) {
                        auto p = tmp.find('\n');
                        ++remaining;
                        tmp = (p == std::string_view::npos) ? std::string_view{} : tmp.substr(p + 1);
                    }
                    std::snprintf(buf, sizeof(buf), "... %d more lines", remaining + 1);
                    rows.push_back(Element{TextElement{
                        .content = buf,
                        .style = Style{}.with_dim(),
                    }});
                    break;
                }

                rows.push_back(Element{TextElement{
                    .content = std::string{line},
                    .style = output_style,
                    .wrap = TextWrap::Wrap,
                }});
                ++line_count;
            }
        }

        return (dsl::v(std::move(rows))
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    struct IconInfo { std::string icon; Color color; };

    // Truncate `s` to at most `max_bytes` without splitting a UTF-8 code
    // point. Good enough for command text which is ASCII in the common
    // case; for the odd non-ASCII arg we avoid printing a broken sequence
    // that would render as replacement glyphs on the next redraw.
    [[nodiscard]] static std::string utf8_trunc(std::string_view s, std::size_t max_bytes) {
        if (s.size() <= max_bytes) return std::string{s};
        std::size_t cut = max_bytes;
        while (cut > 0
               && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        return std::string{s.substr(0, cut)};
    }

    // Build a single-line header "$ command ... elapsed" that fits in w.
    // Layout: `$ ` (green prompt) + command (truncated with `…` if needed)
    //        + right-aligned elapsed suffix. Total never exceeds w, so the
    //        elapsed time is always visible even for very long commands.
    [[nodiscard]] static Element build_command_row(std::string_view cmd,
                                                   std::string_view suffix,
                                                   int w) {
        constexpr std::size_t kPromptBytes = 2;            // "$ "
        constexpr std::string_view kEllipsis = "\xe2\x80\xa6";  // …
        int avail = w - static_cast<int>(kPromptBytes)
                      - static_cast<int>(suffix.size());
        if (avail < 1) avail = 1;

        std::string shown_cmd;
        if (static_cast<int>(cmd.size()) <= avail) {
            shown_cmd = std::string{cmd};
        } else {
            // Leave 1 display column for the ellipsis (3 bytes in UTF-8).
            std::size_t keep = avail > 1 ? static_cast<std::size_t>(avail - 1) : 0;
            shown_cmd = utf8_trunc(cmd, keep);
            shown_cmd += std::string{kEllipsis};
        }

        std::string content;
        content.reserve(kPromptBytes + shown_cmd.size() + suffix.size() + 8);
        content += "$ ";
        std::size_t cmd_off = content.size();
        content += shown_cmd;
        std::size_t suffix_off = content.size();
        if (!suffix.empty()) content += std::string{suffix};

        std::vector<StyledRun> runs;
        runs.push_back(StyledRun{0, kPromptBytes,
            Style{}.with_bold().with_fg(Color::green())});
        if (!shown_cmd.empty()) {
            runs.push_back(StyledRun{cmd_off, shown_cmd.size(), Style{}});
        }
        if (!suffix.empty()) {
            runs.push_back(StyledRun{suffix_off, suffix.size(),
                Style{}.with_dim()});
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case BashStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case BashStatus::Running:
                return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case BashStatus::Success:
                return {"\xe2\x9c\x93", Color::green()};          // ✓
            case BashStatus::Failed:
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
