#pragma once
// maya::widget::SymbolOutline — document symbol tree (background-free)
//
// The outline panel: a nested list of the file's symbols, each with its kind
// glyph (shared with Breadcrumb), tree guides for depth, an optional dim detail
// (a signature or type), and a highlighted current symbol. Feed it a flat list
// of (kind, name, depth) entries — exactly what an LSP documentSymbol response
// flattens to.
//
// Foreground-only.
//
// Usage:
//   SymbolOutline out;
//   out.node(SymKind::Namespace, "maya", 0)
//      .node(SymKind::Class,     "Rope", 1)
//      .node(SymKind::Field,     "text", 2, ": std::string")
//      .node(SymKind::Method,    "at",   2, "(size_t) -> char")
//      .active(3);
//   Element ui = out;

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

struct SymbolOutlineTheme {
    Color name        = Color::hex(0xBAC2DE); // symbol name
    Color active_name = Color::hex(0xF5F5F7); // current symbol name
    Color active_mark = Color::hex(0x89B4FA); // ▸ current-symbol pointer
    Color detail      = Color::hex(0x585B70); // dim signature/type
    Color guide       = Color::hex(0x313244); // tree guides
};

struct SymbolOutline {
    struct Node {
        SymKind     kind;
        std::string name;
        int         depth  = 0;
        std::string detail;
    };

    std::vector<Node>  nodes;
    int                active_ = -1;
    SymbolOutlineTheme theme;

    SymbolOutline& node(SymKind k, std::string name, int depth = 0,
                        std::string detail = {}) {
        nodes.push_back({k, std::move(name), depth, std::move(detail)});
        return *this;
    }
    SymbolOutline& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(nodes.size());

        for (size_t i = 0; i < nodes.size(); ++i) {
            const Node& nd = nodes[i];
            const bool on = (static_cast<int>(i) == active_);

            std::string s; std::vector<StyledRun> runs;
            auto put = [&](std::string_view t, Style st) {
                if (t.empty()) return;
                runs.push_back({s.size(), t.size(), st});
                s += t;
            };

            // current-symbol pointer, then tree guides per depth level
            put(on ? "\xe2\x96\xb8 " : "  ", Style{}.with_fg(theme.active_mark)); // ▸
            for (int d = 0; d < nd.depth; ++d)
                put("\xe2\x94\x82 ", Style{}.with_fg(theme.guide)); // │

            put(std::string(sym_glyph(nd.kind)) + " ",
                Style{}.with_fg(sym_color(nd.kind)));

            Style ns = Style{}.with_fg(on ? theme.active_name : theme.name);
            if (on) ns = ns.with_bold();
            put(nd.name, ns);

            if (!nd.detail.empty())
                put(" " + nd.detail, Style{}.with_fg(theme.detail));

            rows.push_back(Element{TextElement{
                .content = std::move(s),
                .style   = Style{},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
