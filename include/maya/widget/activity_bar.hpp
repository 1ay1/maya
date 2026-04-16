#pragma once
// maya::widget::activity_bar — Bottom status/activity bar
//
// Displays model name, token counts, cost, and context usage in a single
// status line, matching Claude Code's status line and Zed's activity bar.
//
// Usage:
//   ActivityBar bar;
//   bar.set_model("claude-sonnet-4");
//   bar.set_tokens(1200, 3400);
//   bar.set_cost(0.03f);
//   bar.set_context_percent(45);
//   auto ui = bar.build();

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct ActivityBar {
    struct Config {
        Config() = default;
        Style separator_style = Style{}.with_dim().with_fg(Color::bright_black());
        Style label_style     = Style{}.with_dim();
        Style value_style     = Style{};
        Style accent_style    = Style{}.with_fg(Color::blue());
    };

    struct Section {
        std::string icon;
        std::string label;
        Style style;
    };

private:
    Config cfg_;
    std::string model_;
    int input_tokens_ = 0;
    int output_tokens_ = 0;
    float cost_ = 0.0f;
    int context_pct_ = 0;
    std::string status_;
    std::vector<Section> sections_;

    static std::string format_tokens(int n) {
        if (n >= 1000000) {
            float v = static_cast<float>(n) / 1000000.0f;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(v));
            return buf;
        }
        if (n >= 1000) {
            float v = static_cast<float>(n) / 1000.0f;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(v));
            return buf;
        }
        return std::to_string(n);
    }

public:
    ActivityBar() : cfg_{} {}
    explicit ActivityBar(Config cfg) : cfg_(std::move(cfg)) {}

    void set_model(std::string_view name) { model_ = std::string{name}; }
    void set_tokens(int input_tokens, int output_tokens) {
        input_tokens_ = input_tokens;
        output_tokens_ = output_tokens;
    }
    void set_cost(float usd) { cost_ = usd; }
    void set_context_percent(int percent) { context_pct_ = percent; }
    void set_status(std::string_view status) { status_ = std::string{status}; }

    void clear_sections() { sections_.clear(); }
    void add_section(std::string icon, std::string label, Style style = {}) {
        sections_.push_back({std::move(icon), std::move(label), style});
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Build divider line via ComponentElement for full width
        auto divider = Element{ComponentElement{
            .render = [sep_style = cfg_.separator_style](int w, int /*h*/) -> Element {
                std::string line;
                for (int i = 0; i < w; ++i) line += "─";
                return Element{TextElement{
                    .content = std::move(line),
                    .style = sep_style,
                }};
            },
            .layout = {},
        }};

        // Build the status line as a single TextElement with StyledRuns
        std::string content;
        std::vector<StyledRun> runs;

        auto append = [&](std::string_view text, Style style) {
            std::size_t offset = content.size();
            content += text;
            runs.push_back({offset, text.size(), style});
        };

        auto append_separator = [&]() {
            append(" │ ", cfg_.separator_style);
        };

        bool has_prev = false;

        // Model section
        if (!model_.empty()) {
            append(" ● ", cfg_.accent_style);
            append(model_, cfg_.accent_style);
            has_prev = true;
        }

        // Tokens section
        if (input_tokens_ > 0 || output_tokens_ > 0) {
            if (has_prev) append_separator();
            std::string tok = "↑" + format_tokens(input_tokens_)
                            + " ↓" + format_tokens(output_tokens_) + " tokens";
            append(tok, cfg_.label_style);
            has_prev = true;
        }

        // Cost section
        if (cost_ > 0.0f) {
            if (has_prev) append_separator();
            char buf[16];
            std::snprintf(buf, sizeof(buf), "$%.2f", static_cast<double>(cost_));
            append(buf, cfg_.value_style);
            has_prev = true;
        }

        // Context section
        if (context_pct_ > 0) {
            if (has_prev) append_separator();
            Style ctx_style;
            if (context_pct_ < 60)
                ctx_style = Style{}.with_fg(Color::green());
            else if (context_pct_ <= 80)
                ctx_style = Style{}.with_fg(Color::yellow());
            else
                ctx_style = Style{}.with_fg(Color::red());
            append("ctx " + std::to_string(context_pct_) + "%", ctx_style);
            has_prev = true;
        }

        // Status section
        if (!status_.empty()) {
            if (has_prev) append_separator();
            append(status_, cfg_.label_style);
            has_prev = true;
        }

        // Custom sections
        for (auto const& sec : sections_) {
            if (has_prev) append_separator();
            Style s = sec.style.empty() ? cfg_.label_style : sec.style;
            if (!sec.icon.empty()) {
                append(sec.icon + " ", s);
            }
            append(sec.label, s);
            has_prev = true;
        }

        auto status_line = Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};

        return dsl::v(
            std::move(divider),
            std::move(status_line)
        ).build();
    }
};

} // namespace maya
