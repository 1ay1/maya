#pragma once
// maya::widget::Breadcrumb — symbol path bar (background-free)
//
// The strip editors show above the code: the file path plus the nested symbol
// the cursor sits in — each hop a kind glyph + name joined by ›. Kinds carry
// their own colour and codicon (namespace, class, function, …). The last crumb
// is highlighted as the current location.
//
// Foreground-only.
//
// Usage:
//   Breadcrumb bc;
//   bc.crumb(SymKind::Folder, "src")
//     .crumb(SymKind::File,   "rope.cpp")
//     .crumb(SymKind::Class,  "Rope")
//     .crumb(SymKind::Method, "concat");
//   Element ui = bc;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "sym_kind.hpp"

namespace maya {

struct BreadcrumbTheme {
    Color current   = Color::hex(0xF5F5F7); // last (active) crumb name
    Color name      = Color::hex(0x9399B2); // other crumb names
    Color separator = Color::hex(0x585B70); // › chevron
};

struct Breadcrumb {
    struct Crumb { SymKind kind; std::string name; };

    std::vector<Crumb> crumbs;
    BreadcrumbTheme    theme;

    Breadcrumb& crumb(SymKind k, std::string name) {
        crumbs.push_back({k, std::move(name)});
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string            s;
        std::vector<StyledRun> runs;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return;
            runs.push_back({s.size(), t.size(), st});
            s += t;
        };

        put(" ", Style{});
        for (size_t i = 0; i < crumbs.size(); ++i) {
            const bool last = (i + 1 == crumbs.size());
            if (i) put(" \xef\x84\x85 ", Style{}.with_fg(theme.separator)); //  ›
            put(std::string(sym_glyph(crumbs[i].kind)) + " ",
                Style{}.with_fg(sym_color(crumbs[i].kind)));
            Style ns = Style{}.with_fg(last ? theme.current : theme.name);
            if (last) ns = ns.with_bold();
            put(crumbs[i].name, ns);
        }

        return Element{TextElement{
            .content = std::move(s),
            .style   = Style{},
            .wrap    = TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};
    }

private:
};

} // namespace maya
