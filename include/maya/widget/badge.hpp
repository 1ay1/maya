#pragma once
// maya::widget::badge — Inline styled label/chip
//
// Renders a bracketed label with preset color themes, matching Zed's Chip.
//
// Usage:
//   auto b = Badge::success("ok");
//   auto ui = h(Badge::info("info"), Badge::error("fail"));

#include <string>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct Badge {
    struct Config {
        Config() = default;
        Style style;
        std::string left_cap  = "[";
        std::string right_cap = "]";
        Style bracket_style   = Style{}.with_dim();
    };

    std::string label;
    Config config;

    Badge(std::string label_)
        : label(std::move(label_)), config{} {}
    Badge(std::string label_, Config cfg)
        : label(std::move(label_)), config(std::move(cfg)) {}

    static Badge success(std::string label) {
        Config c; c.style = Style{}.with_fg(Color::green());
        return Badge{std::move(label), std::move(c)};
    }

    static Badge error(std::string label) {
        Config c; c.style = Style{}.with_fg(Color::red());
        return Badge{std::move(label), std::move(c)};
    }

    static Badge warning(std::string label) {
        Config c; c.style = Style{}.with_fg(Color::yellow());
        return Badge{std::move(label), std::move(c)};
    }

    static Badge info(std::string label) {
        Config c; c.style = Style{}.with_fg(Color::blue());
        return Badge{std::move(label), std::move(c)};
    }

    static Badge tool(std::string label) {
        Config c; c.style = Style{}.with_fg(Color::magenta());
        return Badge{std::move(label), std::move(c)};
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string content;
        std::vector<StyledRun> runs;

        // Left bracket
        std::size_t off = 0;
        content += config.left_cap;
        runs.push_back({off, config.left_cap.size(), config.bracket_style});

        // Label
        off = content.size();
        content += label;
        runs.push_back({off, label.size(), config.style});

        // Right bracket
        off = content.size();
        content += config.right_cap;
        runs.push_back({off, config.right_cap.size(), config.bracket_style});

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
