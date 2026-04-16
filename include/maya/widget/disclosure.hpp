#pragma once
// maya::widget::disclosure — Chevron-based collapse/expand control
//
// A Zed-style disclosure triangle that toggles content visibility.
//
// Usage:
//   Disclosure d({"Section"});
//   auto ui = d.build(some_content);   // header + content when open
//   d.toggle();                        // collapse/expand

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct Disclosure {
    struct Config {
        Config() = default;
        std::string label;
        Style label_style  = Style{}.with_dim();
        std::string open_icon   = "\xe2\x96\xbc";   // ▼
        std::string closed_icon = "\xe2\x96\xb6";    // ▶
        Style icon_style   = Style{}.with_dim();
    };

private:
    Config cfg_;
    bool open_ = false;

public:
    explicit Disclosure(Config cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] bool is_open() const { return open_; }
    void set_open(bool open) { open_ = open; }
    void toggle() { open_ = !open_; }

    /// Build just the header line (icon + label).
    [[nodiscard]] Element build() const {
        return build_header();
    }

    /// Build header + content when open. When closed, just the header.
    [[nodiscard]] Element build(Element content) const {
        if (!open_) {
            return build_header();
        }

        auto header = build_header();

        // Content with a left-border indent (│ style) like Zed
        auto indent_color = Color::bright_black();
        auto bordered_content = dsl::h(
            Element{TextElement{
                .content = "\xe2\x94\x82 ",  // "│ "
                .style = Style{}.with_fg(indent_color),
            }},
            std::move(content)
        ).build();

        return dsl::v(
            std::move(header),
            std::move(bordered_content)
        ).build();
    }

private:
    [[nodiscard]] Element build_header() const {
        const auto& icon = open_ ? cfg_.open_icon : cfg_.closed_icon;

        std::string content = icon + " " + cfg_.label;

        std::vector<StyledRun> runs;
        std::size_t icon_len = icon.size();
        std::size_t space_len = 1;
        std::size_t label_len = cfg_.label.size();

        // Icon run
        runs.push_back(StyledRun{
            .byte_offset = 0,
            .byte_length = icon_len,
            .style = cfg_.icon_style,
        });

        // Space run (inherit icon style)
        runs.push_back(StyledRun{
            .byte_offset = icon_len,
            .byte_length = space_len,
            .style = cfg_.icon_style,
        });

        // Label run
        runs.push_back(StyledRun{
            .byte_offset = icon_len + space_len,
            .byte_length = label_len,
            .style = cfg_.label_style,
        });

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
