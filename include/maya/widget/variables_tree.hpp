#pragma once
// maya::widget::VariablesTree — debugger variables/watch (background-free)
//
// The debugger Variables/Watch panel: a tree of name = value pairs with types,
// expand chevrons on aggregates, and value colouring by kind (number / string /
// bool / null / pointer). Changed values can be flagged to highlight.
//
// Usage:
//   VariablesTree v;
//   v.scope("Locals")
//    .var("i", "3", VarKind::Number, 1)
//    .var("rope", "Rope*", VarKind::Pointer, 1, /*expandable=*/true, /*open=*/true)
//      .var("weight", "8", VarKind::Number, 2, false, false, /*changed=*/true)
//      .var("text", "\"hello\"", VarKind::String, 2);
//   Element ui = v | dsl::width(44);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class VarKind : uint8_t { Number, String, Bool, Null, Pointer, Other };

struct VariablesTreeTheme {
    Color scope  = Color::hex(0x585B70);
    Color name   = Color::hex(0xBAC2DE);
    Color guide  = Color::hex(0x45475A);
    Color number = Color::hex(0xFAB387);
    Color string = Color::hex(0xA6E3A1);
    Color boolean = Color::hex(0xCBA6F7);
    Color null    = Color::hex(0x6C7086);
    Color pointer = Color::hex(0x89DCEB);
    Color changed = Color::hex(0xF9E2AF);
    Color eq       = Color::hex(0x585B70);
};

class VariablesTree {
public:
    VariablesTree& scope(std::string name) {
        rows_.push_back({true, {}, {}, VarKind::Other, 0, false, false, false, std::move(name)});
        return *this;
    }
    VariablesTree& var(std::string name, std::string value, VarKind kind, int depth,
                       bool expandable = false, bool open = false, bool changed = false) {
        rows_.push_back({false, std::move(name), std::move(value), kind, depth,
                         expandable, open, changed, {}});
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (const auto& r : rows_) out.push_back(row(r));
        return dsl::v(std::move(out)).build();
    }

private:
    struct R { bool scope; std::string name, value; VarKind kind; int depth;
               bool expandable, open, changed; std::string scope_name; };
    std::vector<R> rows_;
    VariablesTreeTheme theme;

    Color vc(VarKind k) const {
        switch (k) { case VarKind::Number: return theme.number; case VarKind::String: return theme.string;
                     case VarKind::Bool: return theme.boolean; case VarKind::Null: return theme.null;
                     case VarKind::Pointer: return theme.pointer; default: return theme.name; }
    }

    Element row(const R& r) const {
        std::string s; std::vector<StyledRun> runs;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            runs.push_back({s.size(),t.size(),st}); s+=t; };
        if (r.scope) {
            put(r.scope_name, Style{}.with_fg(theme.scope).with_bold());
            return Element{TextElement{ .content=std::move(s), .style=Style{},
                                        .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
        }
        for (int d = 0; d < r.depth; ++d) put("  ", Style{});
        put(r.expandable ? (r.open ? "\xef\x84\x87 " : "\xef\x84\x85 ") : "  ", // chevrons
            Style{}.with_fg(theme.guide));
        put(r.name, Style{}.with_fg(r.changed ? theme.changed : theme.name));
        put(" = ", Style{}.with_fg(theme.eq));
        put(r.value, Style{}.with_fg(r.changed ? theme.changed : vc(r.kind)));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
    }
};

} // namespace maya
