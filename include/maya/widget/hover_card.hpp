#pragma once
// maya::widget::HoverCard — hover / documentation popup (background-free)
//
// The tooltip that appears over a symbol: a code-styled signature header, a
// full-width divider, a wrapped documentation body, and optional dim footnotes
// (source module, since-version, …). Thin rounded border.
//
// Usage:
//   HoverCard h;
//   h.signature("char Rope::at(std::size_t i) const")
//    .doc("Random access into the rope by flat index. O(log n).")
//    .note("rope.hpp").note("since 0.3");
//   Element ui = h | dsl::width(52);

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

struct HoverCardTheme {
    Color border    = Color::hex(0x313244);
    Color signature = Color::hex(0xCBA6F7); // header
    Color divider   = Color::hex(0x313244);
    Color doc        = Color::hex(0xBAC2DE); // body
    Color note       = Color::hex(0x585B70); // footnotes
};

struct HoverCard {
    std::string              signature_;
    std::string              doc_;
    std::vector<std::string> notes_;
    HoverCardTheme           theme;

    HoverCard& signature(std::string s) { signature_ = std::move(s); return *this; }
    HoverCard& doc(std::string d)       { doc_ = std::move(d); return *this; }
    HoverCard& note(std::string n)      { notes_.push_back(std::move(n)); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> col;

        if (!signature_.empty())
            col.push_back(Element{TextElement{
                .content = signature_,
                .style = Style{}.with_fg(theme.signature).with_bold(),
                .wrap = TextWrap::NoWrap }});

        if (!doc_.empty()) {
            col.push_back(rule());
            col.push_back(Element{TextElement{
                .content = doc_, .style = Style{}.with_fg(theme.doc),
                .wrap = TextWrap::Wrap }});
        }

        if (!notes_.empty()) {
            std::string s; std::vector<StyledRun> r;
            for (size_t i = 0; i < notes_.size(); ++i) {
                if (i) { r.push_back({s.size(), 3, Style{}.with_fg(theme.divider)});
                         s += " \xc2\xb7 "; }                       // ·
                r.push_back({s.size(), notes_[i].size(), Style{}.with_fg(theme.note)});
                s += notes_[i];
            }
            col.push_back(rule());
            col.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                               .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(theme.border)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(col)).build());
    }

private:
    // A full-width horizontal divider that fills the card's inner width.
    Element rule() const {
        return Element{ComponentElement{
            .render = [c = theme.divider](int w, int) -> Element {
                std::string s;
                for (int i = 0; i < w; ++i) s += "\xe2\x94\x80"; // ─
                return Element{TextElement{ .content = std::move(s),
                                            .style = Style{}.with_fg(c),
                                            .wrap = TextWrap::NoWrap }};
            },
        }};
    }
};

} // namespace maya
