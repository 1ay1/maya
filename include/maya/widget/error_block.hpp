#pragma once
// maya::widget::error_block — Structured error/exception display
//
// Displays errors with type classification, message, and optional
// details/stack trace in a red-tinted bordered card.
//
//   ErrorBlock err("APIError", "Rate limit exceeded");
//   err.set_detail("Retry after 30 seconds");
//   err.set_hint("Consider using a lower rate or batching requests");
//   auto ui = err.build();

#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ErrorSeverity : uint8_t {
    Error,    // red
    Warning,  // yellow
    Info,     // blue
};

class ErrorBlock {
    std::string error_type_;
    std::string message_;
    std::string detail_;
    std::string hint_;
    std::vector<std::string> trace_;
    ErrorSeverity severity_ = ErrorSeverity::Error;
    bool show_trace_ = false;

    [[nodiscard]] Color severity_color() const {
        switch (severity_) {
            case ErrorSeverity::Error:   return Color::rgb(224, 108, 117);
            case ErrorSeverity::Warning: return Color::rgb(229, 192, 123);
            case ErrorSeverity::Info:    return Color::rgb(97, 175, 239);
        }
        return Color::rgb(224, 108, 117);
    }

    [[nodiscard]] const char* severity_icon() const {
        switch (severity_) {
            case ErrorSeverity::Error:   return "\xe2\x9c\x97"; // ✗
            case ErrorSeverity::Warning: return "\xe2\x9a\xa0"; // ⚠
            case ErrorSeverity::Info:    return "\xe2\x93\x98"; // ⓘ
        }
        return "\xe2\x9c\x97";
    }

public:
    ErrorBlock() = default;
    ErrorBlock(std::string type, std::string message)
        : error_type_(std::move(type)), message_(std::move(message)) {}

    void set_type(std::string t) { error_type_ = std::move(t); }
    void set_message(std::string m) { message_ = std::move(m); }
    void set_detail(std::string d) { detail_ = std::move(d); }
    void set_hint(std::string h) { hint_ = std::move(h); }
    void set_severity(ErrorSeverity s) { severity_ = s; }
    void set_show_trace(bool b) { show_trace_ = b; }
    void add_trace_line(std::string line) { trace_.push_back(std::move(line)); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        Color sc = severity_color();
        auto msg_style = Style{}.with_fg(Color::rgb(200, 204, 212));
        auto dim       = Style{}.with_fg(Color::rgb(92, 99, 112));
        auto hint_style = Style{}.with_fg(Color::rgb(152, 195, 121)).with_italic();

        std::vector<Element> rows;

        // Error type + message
        {
            std::vector<Element> header;
            if (!error_type_.empty()) {
                header.push_back(text(error_type_, Style{}.with_fg(sc).with_bold()));
                header.push_back(text(": ", dim));
            }
            header.push_back(text(message_, msg_style));
            rows.push_back(h(std::move(header)).build());
        }

        // Detail
        if (!detail_.empty()) {
            rows.push_back(text(detail_, Style{}.with_fg(Color::rgb(171, 178, 191))));
        }

        // Hint
        if (!hint_.empty()) {
            rows.push_back(h(
                text("hint: ", hint_style),
                text(hint_, Style{}.with_fg(Color::rgb(152, 195, 121)))
            ).build());
        }

        // Stack trace
        if (show_trace_ && !trace_.empty()) {
            rows.push_back(text(""));
            for (auto& line : trace_) {
                rows.push_back(h(
                    text("  ", dim),
                    text(line, dim)
                ).build());
            }
        }

        // Border label
        std::string label = std::string(" ") + severity_icon() + " "
            + (severity_ == ErrorSeverity::Error ? "Error"
             : severity_ == ErrorSeverity::Warning ? "Warning"
             : "Info") + " ";

        Color border_col = Color::rgb(
            static_cast<uint8_t>(sc.r() / 2 + 30),
            static_cast<uint8_t>(sc.g() / 4 + 15),
            static_cast<uint8_t>(sc.b() / 4 + 15));

        return (v(std::move(rows))
            | border(BorderStyle::Round)
            | bcolor(border_col)
            | btext(std::move(label))
            | padding(0, 1)).build();
    }
};

} // namespace maya
