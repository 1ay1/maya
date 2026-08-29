#pragma once
// maya::widget::DiffHunkView — unified diff hunk viewer (background-free)
//
// A git-style unified diff: old/new line-number gutters, a +/-/·  change
// marker, and per-line colouring (added green, removed red, context dim). Hunk
// headers (@@ … @@) render as their own accent row. Foreground-only, so it
// reads on any terminal background.
//
// Usage:
//   DiffHunkView d;
//   d.hunk("@@ -10,6 +10,7 @@ char Rope::at(size_t i)")
//    .ctx("    if (i < weight && left)", 10, 10)
//    .del("        return left->at(i);", 11)
//    .add("        return left->at(i);   // fast path", 11)
//    .ctx("    return text[i - weight];", 12, 13);
//   Element ui = d;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct DiffHunkTheme {
    Color gutter = Color::hex(0x484F58); // line numbers
    Color hunk   = Color::hex(0x89DCEB); // @@ header
    Color add     = Color::hex(0xA6E3A1); // + lines
    Color del     = Color::hex(0xF38BA8); // - lines
    Color context = Color::hex(0x9399B2); // unchanged lines
    Color add_mk   = Color::hex(0x3FB950); // + marker
    Color del_mk   = Color::hex(0xF85149); // - marker
};

struct DiffHunkView {
    enum class Kind : uint8_t { Context, Add, Del, Hunk };
    struct Line { Kind kind; std::string text; int old_no; int new_no; };

    std::vector<Line> lines;
    DiffHunkTheme     theme;
    int               gutter_w = 4;

    DiffHunkView& hunk(std::string header) {
        lines.push_back({Kind::Hunk, std::move(header), -1, -1}); return *this;
    }
    DiffHunkView& ctx(std::string t, int oldn, int newn) {
        lines.push_back({Kind::Context, std::move(t), oldn, newn}); return *this;
    }
    DiffHunkView& add(std::string t, int newn) {
        lines.push_back({Kind::Add, std::move(t), -1, newn}); return *this;
    }
    DiffHunkView& del(std::string t, int oldn) {
        lines.push_back({Kind::Del, std::move(t), oldn, -1}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(lines.size());
        for (const auto& ln : lines) rows.push_back(row(ln));
        return dsl::v(std::move(rows)).build();
    }

private:
    std::string num(int n) const {
        std::string s = (n < 0) ? "" : std::to_string(n);
        if (static_cast<int>(s.size()) < gutter_w)
            s.insert(s.begin(), gutter_w - s.size(), ' ');
        return s;
    }

    Element row(const Line& ln) const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
        };

        if (ln.kind == Kind::Hunk) {
            put(ln.text, Style{}.with_fg(theme.hunk));
            return Element{TextElement{ .content = std::move(s), .style = Style{},
                                        .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
        }

        const Style gut = Style{}.with_fg(theme.gutter);
        Color txt, mkc; const char* mk;
        switch (ln.kind) {
            case Kind::Add: txt = theme.add; mkc = theme.add_mk; mk = "+"; break;
            case Kind::Del: txt = theme.del; mkc = theme.del_mk; mk = "-"; break;
            default:        txt = theme.context; mkc = theme.gutter; mk = " "; break;
        }

        put(num(ln.old_no), gut);
        put(" ", Style{});
        put(num(ln.new_no), gut);
        put(" ", Style{});
        put(mk, Style{}.with_fg(mkc).with_bold());
        put(" ", Style{});
        put(ln.text, Style{}.with_fg(txt));

        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
