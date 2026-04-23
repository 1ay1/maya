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

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class EditStatus : uint8_t { Pending, Applying, Applied, Failed };

class EditTool {
public:
    struct EditPair {
        std::string old_text;
        std::string new_text;
    };

private:
    std::string file_path_;
    // Multi-edit storage. set_old_text/set_new_text writes into edits_[0],
    // creating it on demand — preserves the single-edit ergonomics for the
    // legacy callers while letting the renderer emit one diff section per
    // edit when the model emits an `edits: [...]` array.
    std::vector<EditPair> edits_;
    EditStatus status_ = EditStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;

    void ensure_first_edit() {
        if (edits_.empty()) edits_.emplace_back();
    }

public:
    EditTool() = default;
    explicit EditTool(std::string path) : file_path_(std::move(path)) {}

    void set_file_path(std::string_view p) { file_path_ = std::string{p}; }
    void set_old_text(std::string_view t) { ensure_first_edit(); edits_[0].old_text = std::string{t}; }
    void set_new_text(std::string_view t) { ensure_first_edit(); edits_[0].new_text = std::string{t}; }
    void set_edits(std::vector<EditPair> v) { edits_ = std::move(v); }
    void add_edit(std::string old_text, std::string new_text) {
        edits_.push_back({std::move(old_text), std::move(new_text)});
    }
    void clear_edits() { edits_.clear(); }
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

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == EditStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        } else if (status_ == EditStatus::Applied) {
            border_color = Color::green();
        }

        std::vector<Element> rows;

        // File path + elapsed
        {
            std::string content = file_path_;
            std::vector<StyledRun> runs;

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

        // Expanded: render one diff section per edit, separated by a header
        // line ("edit N/M"). Empty edits (e.g. mid-stream where new_text
        // hasn't started arriving) still render the old_text side so the
        // user sees progress for every edit, not just the first.
        bool any_content = false;
        for (const auto& e : edits_) {
            if (!e.old_text.empty() || !e.new_text.empty()) { any_content = true; break; }
        }
        if (expanded_ && any_content) {
            auto separator = [](std::optional<std::string> label = std::nullopt) {
                return Element{ComponentElement{
                    .render = [lbl = std::move(label)](int w, int /*h*/) -> Element {
                        std::string line;
                        if (lbl && !lbl->empty()) {
                            std::string mid = " " + *lbl + " ";
                            int side = std::max(0, (w - static_cast<int>(mid.size())) / 2);
                            for (int i = 0; i < side; ++i) line += "\xe2\x94\x88";
                            line += mid;
                            int rest = std::max(0, w - static_cast<int>(line.size() - (mid.size() - mid.size())));
                            // crude: pad to width
                            int used = side + static_cast<int>(mid.size());
                            for (int i = used; i < w; ++i) line += "\xe2\x94\x88";
                            (void)rest;
                        } else {
                            for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";
                        }
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = Style{}.with_dim(),
                        }};
                    },
                    .layout = {},
                }};
            };

            auto remove_style = Style{}.with_fg(Color::red());
            auto add_style = Style{}.with_fg(Color::green());
            auto bg_remove = Style{}.with_fg(Color::red()).with_dim();
            auto bg_add = Style{}.with_fg(Color::green()).with_dim();

            auto push_lines = [&](std::string_view sv, char marker, Style mark_style, Style text_style) {
                while (!sv.empty()) {
                    auto nl = sv.find('\n');
                    auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                    sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                    std::string content;
                    content += marker;
                    content += ' ';
                    content += line;
                    std::vector<StyledRun> runs;
                    runs.push_back(StyledRun{0, 2, mark_style});
                    runs.push_back(StyledRun{2, line.size(), text_style});

                    rows.push_back(Element{TextElement{
                        .content = std::move(content),
                        .style = {},
                        .wrap = TextWrap::Wrap,
                        .runs = std::move(runs),
                    }});
                }
            };

            const std::size_t n = edits_.size();
            for (std::size_t i = 0; i < n; ++i) {
                const auto& e = edits_[i];
                if (n == 1) {
                    rows.push_back(separator());
                } else {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "edit %zu/%zu", i + 1, n);
                    rows.push_back(separator(std::string{buf}));
                }
                if (!e.old_text.empty()) push_lines(e.old_text, '-', bg_remove, remove_style);
                if (!e.new_text.empty()) push_lines(e.new_text, '+', bg_add,    add_style);
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

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case EditStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case EditStatus::Applying:
                return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case EditStatus::Applied:
                return {"\xe2\x9c\x93", Color::green()};          // ✓
            case EditStatus::Failed:
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
