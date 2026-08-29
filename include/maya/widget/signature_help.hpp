#pragma once
// maya::widget::SignatureHelp — function signature popup (background-free)
//
// The tooltip shown while typing a call: the function signature with the
// parameter at the cursor highlighted, an overload counter, and an optional
// doc line. Thin rounded border.
//
// Usage:
//   SignatureHelp sh{"concat", "Rope*"};
//   sh.param("Rope* a").param("Rope* b").active(1)
//     .overload(1, 2).doc("Join two ropes into one.");
//   Element ui = sh;

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

struct SignatureHelpTheme {
    Color name    = Color::hex(0x89B4FA); // function name
    Color param   = Color::hex(0x9399B2); // inactive parameters
    Color active   = Color::hex(0xF9E2AF); // active parameter
    Color punct    = Color::hex(0x6C7086); // ( , ) -> punctuation
    Color ret       = Color::hex(0x94E2D5); // return type
    Color counter   = Color::hex(0x585B70); // overload counter
    Color doc        = Color::hex(0x9399B2); // doc string
};

struct SignatureHelp {
    std::string          name;
    std::string          ret;
    std::vector<std::string> params;
    int                  active_ = 0;
    int                  ovl_i = 0, ovl_n = 0;
    std::string          doc_;
    SignatureHelpTheme   theme;

    SignatureHelp() = default;
    SignatureHelp(std::string n, std::string r = {}) : name(std::move(n)), ret(std::move(r)) {}

    SignatureHelp& param(std::string p)    { params.push_back(std::move(p)); return *this; }
    SignatureHelp& active(int i)           { active_ = i; return *this; }
    SignatureHelp& overload(int i, int n)  { ovl_i = i; ovl_n = n; return *this; }
    SignatureHelp& doc(std::string d)      { doc_ = std::move(d); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return;
            r.push_back({s.size(), t.size(), st});
            s += t;
        };

        put(name, Style{}.with_fg(theme.name).with_bold());
        put("(", Style{}.with_fg(theme.punct));
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) put(", ", Style{}.with_fg(theme.punct));
            const bool on = (static_cast<int>(i) == active_);
            Style ps = on ? Style{}.with_fg(theme.active).with_bold().with_underline()
                          : Style{}.with_fg(theme.param);
            put(params[i], ps);
        }
        put(")", Style{}.with_fg(theme.punct));
        if (!ret.empty()) {
            put(" \xe2\x86\x92 ", Style{}.with_fg(theme.punct)); // →
            put(ret, Style{}.with_fg(theme.ret));
        }

        Element sig{TextElement{ .content = std::move(s), .style = Style{},
                                 .wrap = TextWrap::NoWrap, .runs = std::move(r) }};

        std::vector<Element> col;
        if (ovl_n > 1) {
            std::string c = std::to_string(ovl_i) + "/" + std::to_string(ovl_n);
            col.push_back(dsl::h(
                sig, dsl::spacer(),
                Element{TextElement{ .content = "  " + c,
                                     .style = Style{}.with_fg(theme.counter),
                                     .wrap = TextWrap::NoWrap }}
            ).build());
        } else {
            col.push_back(std::move(sig));
        }
        if (!doc_.empty()) {
            col.push_back(Element{TextElement{
                .content = doc_, .style = Style{}.with_fg(theme.doc).with_italic(),
                .wrap = TextWrap::Wrap }});
        }

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(Color::hex(0x313244))
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(col)).build());
    }
};

} // namespace maya
