#pragma once
// maya::widget::callout — Severity-based alert/notification
//
// Minimal callout matching Zed's agent panel style: severity icon + title
// on the header line, then description lines with a left "│" border in
// the severity color.
//
// Usage:
//   auto err = Callout::error("Failed to read file", "permission denied");
//   auto info = Callout::info("3 files modified");
//   auto ui = v(err, info);

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// Severity — alert level for callouts
// ============================================================================

enum class Severity : uint8_t { Info, Success, Warning, Error };

// ============================================================================
// Callout — severity-based notification widget
// ============================================================================

class Callout {
public:
    struct Config {
        Severity severity    = Severity::Info;
        std::string title;
        std::string description;
    };

    explicit Callout(Config cfg)
        : cfg_(std::move(cfg)) {}

    Callout(Severity sev, std::string title, std::string desc = "") {
        cfg_.severity = sev;
        cfg_.title = std::move(title);
        cfg_.description = std::move(desc);
    }

    static Callout info(std::string title, std::string desc = "") {
        return Callout(Severity::Info, std::move(title), std::move(desc));
    }
    static Callout success(std::string title, std::string desc = "") {
        return Callout(Severity::Success, std::move(title), std::move(desc));
    }
    static Callout warning(std::string title, std::string desc = "") {
        return Callout(Severity::Warning, std::move(title), std::move(desc));
    }
    static Callout error(std::string title, std::string desc = "") {
        return Callout(Severity::Error, std::move(title), std::move(desc));
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, color] = severity_props(cfg_.severity);
        Style header_style = Style{}.with_bold().with_fg(color);
        Style border_style = Style{}.with_fg(color);

        // Header: "  ✗ Error title"
        auto header = Element{TextElement{
            .content = "  " + std::string{icon} + " " + cfg_.title,
            .style = header_style,
        }};

        if (cfg_.description.empty()) {
            return header;
        }

        // Split description into lines and prefix each with "  │ "
        std::vector<Element> rows;
        rows.push_back(std::move(header));

        std::string_view remaining{cfg_.description};
        while (!remaining.empty()) {
            auto nl = remaining.find('\n');
            std::string_view line;
            if (nl == std::string_view::npos) {
                line = remaining;
                remaining = {};
            } else {
                line = remaining.substr(0, nl);
                remaining = remaining.substr(nl + 1);
            }

            // "  │ line content"
            std::string content = "  \u2502 " + std::string{line};

            // Build with styled runs: border char in severity color, text dimmed
            std::size_t border_end = 5;  // "  │ " is 5 bytes (2 spaces + 3-byte UTF-8 + space)
            std::size_t content_len = content.size();
            auto text_style = Style{};

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .runs = {
                    StyledRun{.byte_offset = 0, .byte_length = border_end, .style = border_style},
                    StyledRun{.byte_offset = border_end,
                              .byte_length = content_len - border_end,
                              .style = text_style},
                },
            }});
        }

        return dsl::v(std::move(rows)).build();
    }

private:
    Config cfg_;

    struct SeverityProps {
        std::string_view icon;
        Color color;
    };

    [[nodiscard]] static constexpr SeverityProps severity_props(Severity sev) noexcept {
        switch (sev) {
            case Severity::Info:
                return {"\u2139", Color::blue()};    // ℹ blue
            case Severity::Success:
                return {"\u2713", Color::green()};   // ✓ green
            case Severity::Warning:
                return {"\u26A0", Color::yellow()};  // ⚠ yellow
            case Severity::Error:
                return {"\u2717", Color::red()};     // ✗ red
        }
        __builtin_unreachable();
    }
};

} // namespace maya
