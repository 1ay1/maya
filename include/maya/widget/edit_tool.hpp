#pragma once
// maya::widget::edit_tool — File edit operation card
//
// Zed's inline diff card + Claude Code's search/replace display.
//
//   ╭─ ✓ Edit ─────────────────────────────╮
//   │ src/render/canvas.cpp          0.1s  │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ - damage_ = {0, 0, width_, height_}; │
//   │ + damage_ = {0, 0, 0, 0};           │
//   │ + // Start with empty damage rect   │
//   ╰──────────────────────────────────────╯

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class EditStatus : uint8_t { Pending, Applying, Applied, Failed };

class EditTool {
    std::string file_path_;
    std::string old_text_;
    std::string new_text_;
    EditStatus status_ = EditStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;

public:
    EditTool() = default;
    explicit EditTool(std::string path) : file_path_(std::move(path)) {}

    void set_file_path(std::string_view p) { file_path_ = std::string{p}; }
    void set_old_text(std::string_view t) { old_text_ = std::string{t}; }
    void set_new_text(std::string_view t) { new_text_ = std::string{t}; }
    void set_status(EditStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }

    [[nodiscard]] EditStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Edit ";

        auto border_color = Color::rgb(50, 54, 62);
        if (status_ == EditStatus::Failed)
            border_color = Color::rgb(120, 60, 65);
        else if (status_ == EditStatus::Applied)
            border_color = Color::rgb(50, 80, 55);

        auto builder = detail::box()
            .border(BorderStyle::Round)
            .border_color(border_color)
            .border_text(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            .padding(0, 1, 0, 1);

        std::vector<Element> rows;

        // File path + elapsed
        {
            std::string content = file_path_;
            std::vector<StyledRun> runs;
            auto path_style = Style{}.with_fg(Color::rgb(171, 178, 191));
            runs.push_back(StyledRun{0, file_path_.size(), path_style});

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

        // Expanded: show old → new diff
        if (expanded_ && (!old_text_.empty() || !new_text_.empty())) {
            // Separator
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim().with_fg(Color::rgb(50, 54, 62)),
                    }};
                },
                .layout = {},
            }});

            auto remove_style = Style{}.with_fg(Color::rgb(224, 108, 117));
            auto add_style = Style{}.with_fg(Color::rgb(152, 195, 121));
            auto bg_remove = Style{}.with_fg(Color::rgb(224, 108, 117)).with_dim();
            auto bg_add = Style{}.with_fg(Color::rgb(152, 195, 121)).with_dim();

            // Old text lines (removals)
            if (!old_text_.empty()) {
                std::string_view sv = old_text_;
                while (!sv.empty()) {
                    auto nl = sv.find('\n');
                    auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                    sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                    std::string content = "- ";
                    content += line;
                    std::vector<StyledRun> runs;
                    runs.push_back(StyledRun{0, 2, bg_remove});
                    runs.push_back(StyledRun{2, line.size(), remove_style});

                    rows.push_back(Element{TextElement{
                        .content = std::move(content),
                        .style = {},
                        .wrap = TextWrap::Wrap,
                        .runs = std::move(runs),
                    }});
                }
            }

            // New text lines (additions)
            if (!new_text_.empty()) {
                std::string_view sv = new_text_;
                while (!sv.empty()) {
                    auto nl = sv.find('\n');
                    auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                    sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                    std::string content = "+ ";
                    content += line;
                    std::vector<StyledRun> runs;
                    runs.push_back(StyledRun{0, 2, bg_add});
                    runs.push_back(StyledRun{2, line.size(), add_style});

                    rows.push_back(Element{TextElement{
                        .content = std::move(content),
                        .style = {},
                        .wrap = TextWrap::Wrap,
                        .runs = std::move(runs),
                    }});
                }
            }
        }

        return std::move(builder)(detail::vstack()(std::move(rows)));
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case EditStatus::Pending:
                return {"\xe2\x97\x8b", Color::rgb(92, 99, 112)};       // ○
            case EditStatus::Applying:
                return {"\xe2\x97\x8f", Color::rgb(229, 192, 123)};     // ●
            case EditStatus::Applied:
                return {"\xe2\x9c\x93", Color::rgb(152, 195, 121)};     // ✓
            case EditStatus::Failed:
                return {"\xe2\x9c\x97", Color::rgb(224, 108, 117)};     // ✗
        }
        return {"\xe2\x97\x8b", Color::rgb(92, 99, 112)};
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
