#pragma once
// maya::widget::CallHierarchy — incoming/outgoing call tree (background-free)
//
// The call-hierarchy view: a root symbol and a tree of callers (incoming) or
// callees (outgoing) with kind glyphs, tree guides, the file:line each call
// site lives at, and depth nesting.
//
// Usage:
//   CallHierarchy h{CallHierarchy::Incoming};
//   h.root(SymKind::Method, "Rope::at")
//    .call(SymKind::Function, "operator[]", 1, "rope.hpp:20")
//    .call(SymKind::Method,   "Editor::char_at", 2, "editor.cpp:88");
//   Element ui = h | dsl::width(52);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "sym_kind.hpp"

namespace maya {

struct CallHierarchyTheme {
    Color root   = Color::hex(0xF5F5F7);
    Color name   = Color::hex(0xBAC2DE);
    Color guide  = Color::hex(0x45475A);
    Color loc     = Color::hex(0x585B70);
    Color arrow    = Color::hex(0x89B4FA);
};

class CallHierarchy {
public:
    enum Dir { Incoming, Outgoing };
    explicit CallHierarchy(Dir d = Incoming) : dir_(d) {}

    CallHierarchy& root(SymKind k, std::string name) {
        rows_.push_back({k, std::move(name), 0, {}}); return *this;
    }
    CallHierarchy& call(SymKind k, std::string name, int depth, std::string loc) {
        rows_.push_back({k, std::move(name), depth, std::move(loc)}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i) out.push_back(row(rows_[i], i == 0));
        return dsl::v(std::move(out)).build();
    }

private:
    struct Node { SymKind kind; std::string name; int depth; std::string loc; };
    Dir              dir_;
    std::vector<Node> rows_;
    CallHierarchyTheme theme;

    Element row(const Node& n, bool is_root) const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        for (int d = 0; d < n.depth; ++d) put("  \xe2\x94\x82", Style{}.with_fg(theme.guide)); // │
        if (n.depth > 0)
            put(dir_ == Incoming ? " \xe2\x86\x90 " : " \xe2\x86\x92 ", // ← incoming / → outgoing
                Style{}.with_fg(theme.arrow));
        else put(" ", Style{});
        put(std::string(sym_glyph(n.kind)) + " ", Style{}.with_fg(sym_color(n.kind)));
        Style ns = Style{}.with_fg(is_root ? theme.root : theme.name);
        if (is_root) ns = ns.with_bold();
        put(n.name, ns);
        if (!n.loc.empty()) put("  " + n.loc, Style{}.with_fg(theme.loc));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
