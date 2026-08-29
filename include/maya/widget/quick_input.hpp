#pragma once
// maya::widget::QuickInput — labeled input box (background-free)
//
// The small modal input editors use for Rename Symbol / Go to Line / Save As:
// a thin rounded box with a label, the current value (or a dim placeholder), a
// block caret, and a right-aligned hint. Turns red on an error message.
//
// Usage:
//   QuickInput q;
//   q.label("Rename Symbol").value("weight").hint("Enter to confirm");
//   Element ui = q | dsl::width(44);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct QuickInputTheme {
    Color border      = Color::hex(0x313244);
    Color border_err  = Color::hex(0xF38BA8);
    Color label       = Color::hex(0x89B4FA);
    Color value       = Color::hex(0xE6EDF3);
    Color placeholder = Color::hex(0x585B70);
    Color caret       = Color::hex(0xF5F5F7);
    Color hint         = Color::hex(0x585B70);
    Color error         = Color::hex(0xF38BA8);
};

struct QuickInput {
    std::string label_;
    std::string value_;
    std::string placeholder_;
    std::string hint_;
    std::string error_;
    QuickInputTheme theme;

    QuickInput& label(std::string s)       { label_ = std::move(s); return *this; }
    QuickInput& value(std::string s)       { value_ = std::move(s); return *this; }
    QuickInput& placeholder(std::string s) { placeholder_ = std::move(s); return *this; }
    QuickInput& hint(std::string s)        { hint_ = std::move(s); return *this; }
    QuickInput& error(std::string s)       { error_ = std::move(s); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;

        // label + hint on one line
        {
            std::string s; std::vector<StyledRun> r;
            auto put = [&](std::string_view t, Style st){ if(t.empty())return;
                r.push_back({s.size(), t.size(), st}); s += t; };
            put(label_, Style{}.with_fg(theme.label).with_bold());
            rows.push_back(dsl::h(
                Element{TextElement{ .content = std::move(s), .style = Style{},
                                     .wrap = TextWrap::NoWrap, .runs = std::move(r) }},
                dsl::spacer(),
                Element{TextElement{ .content = hint_,
                                     .style = Style{}.with_fg(theme.hint),
                                     .wrap = TextWrap::NoWrap }}
            ).build());
        }

        // value line with caret
        {
            std::string s; std::vector<StyledRun> r;
            auto put = [&](std::string_view t, Style st){ if(t.empty())return;
                r.push_back({s.size(), t.size(), st}); s += t; };
            if (value_.empty())
                put(placeholder_, Style{}.with_fg(theme.placeholder).with_italic());
            else
                put(value_, Style{}.with_fg(theme.value));
            put(" ", Style{}.with_inverse()); // block caret
            rows.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                                .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }

        if (!error_.empty())
            rows.push_back(Element{TextElement{
                .content = "\xef\x81\xb1 " + error_, //  warning
                .style = Style{}.with_fg(theme.error), .wrap = TextWrap::Wrap }});

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(error_.empty() ? theme.border : theme.border_err)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(rows)).build());
    }
};

} // namespace maya
