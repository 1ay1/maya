#pragma once
// maya::widget::DiagnosticLens — inline diagnostic (squiggle + message)
//
// Renders an editor diagnostic the way modern IDEs do: an underline "squiggle"
// aligned under the offending span, followed by an indented message row with a
// severity glyph and colour. Compose it directly beneath a CodeView line to
// get the classic error-lens look. Foreground-only.
//
// Usage:
//   DiagnosticLens d{Severity::Error, "expected ';' after expression"};
//   d.at(/*col=*/14, /*len=*/1).gutter(6);   // align under column 14
//   Element ui = v(code_line, d);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class Severity : uint8_t { Error, Warning, Info, Hint };

struct DiagnosticLens {
    Severity    severity = Severity::Error;
    std::string message;
    int         col_    = 0;   // 0-based column where the span starts
    int         len_    = 1;   // span length in columns
    int         gutter_ = 0;   // left indent (e.g. width of the code gutter)
    bool        squiggle = true;

    DiagnosticLens(Severity s, std::string msg)
        : severity(s), message(std::move(msg)) {}

    DiagnosticLens& at(int col, int len) { col_ = col; len_ = std::max(1, len); return *this; }
    DiagnosticLens& gutter(int g)         { gutter_ = g; return *this; }
    DiagnosticLens& no_squiggle()         { squiggle = false; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const Color c = color();
        const Style sc = Style{}.with_fg(c);

        std::vector<Element> rows;

        if (squiggle) {
            // spaces up to (gutter + col), then a run of ~ under the span
            std::string s(static_cast<size_t>(gutter_ + col_), ' ');
            std::vector<StyledRun> runs;
            if (!s.empty()) runs.push_back({0, s.size(), Style{}});
            std::string sq(static_cast<size_t>(len_), '~');
            runs.push_back({s.size(), sq.size(), sc});
            s += sq;
            rows.push_back(Element{TextElement{
                .content = std::move(s),
                .style   = Style{},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }

        // message row: <indent> ╰─ <glyph> message   (connector under the span)
        std::string m(static_cast<size_t>(gutter_ + col_), ' ');
        std::vector<StyledRun> mruns;
        if (!m.empty()) mruns.push_back({0, m.size(), Style{}});
        std::string tail = (squiggle ? "\xe2\x95\xb0\xe2\x94\x80 " : "") // ╰─
                         + std::string(glyph()) + " " + message;
        mruns.push_back({m.size(), tail.size(), sc});
        m += tail;
        rows.push_back(Element{TextElement{
            .content = std::move(m),
            .style   = Style{},
            .wrap    = TextWrap::NoWrap,
            .runs    = std::move(mruns),
        }});

        return dsl::v(std::move(rows)).build();
    }

private:
    Color color() const {
        switch (severity) {
            case Severity::Error:   return Color::hex(0xF38BA8);
            case Severity::Warning: return Color::hex(0xF9E2AF);
            case Severity::Info:    return Color::hex(0x89B4FA);
            case Severity::Hint:    return Color::hex(0x94E2D5);
        }
        return Color::hex(0xF38BA8);
    }
    const char* glyph() const {
        switch (severity) {
            case Severity::Error:   return "\xef\x81\x97"; //  times-circle
            case Severity::Warning: return "\xef\x81\xb1"; //  warning
            case Severity::Info:    return "\xef\x81\x9a"; //  info-circle
            case Severity::Hint:    return "\xef\x83\xab"; //  lightbulb
        }
        return "\xe2\x86\xb3"; // ↳
    }
};

} // namespace maya
