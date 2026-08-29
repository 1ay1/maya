#pragma once
// maya::widget::CallStack — debugger call stack (background-free)
//
// The Call Stack panel: stack frames top-to-bottom with the current frame
// marked (▶ accent), the function name, and a dim file:line. Library/external
// frames render dimmed.
//
// Usage:
//   CallStack c;
//   c.frame("Rope::at", "rope.hpp:12", /*current=*/true)
//    .frame("Editor::char_at", "editor.cpp:88")
//    .frame("main", "main.cpp:20")
//    .frame("__libc_start_main", "libc.so", /*current=*/false, /*external=*/true);
//   Element ui = c | dsl::width(48);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct CallStackTheme {
    Color current  = Color::hex(0xF9E2AF);
    Color name     = Color::hex(0xBAC2DE);
    Color loc      = Color::hex(0x585B70);
    Color external = Color::hex(0x494D64);
    Color marker    = Color::hex(0xF9E2AF);
};

class CallStack {
public:
    CallStack& frame(std::string name, std::string loc, bool current = false, bool external = false) {
        rows_.push_back({std::move(name), std::move(loc), current, external}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (const auto& r : rows_) out.push_back(row(r));
        return dsl::v(std::move(out)).build();
    }

private:
    struct F { std::string name, loc; bool current, external; };
    std::vector<F> rows_;
    CallStackTheme theme;

    Element row(const F& f) const {
        std::string left; std::vector<StyledRun> lr;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(),t.size(),st}); left+=t; };
        put(f.current ? "\xe2\x96\xb6 " : "  ", Style{}.with_fg(theme.marker)); // ▶
        Color nc = f.external ? theme.external : f.current ? theme.current : theme.name;
        Style ns = Style{}.with_fg(nc); if (f.current) ns = ns.with_bold();
        put(f.name, ns);
        std::string loc = f.loc;
        return Element{ComponentElement{
            .render=[left=std::move(left), lr=std::move(lr), loc=std::move(loc),
                     ext=f.external, lc=theme.loc, xc=theme.external](int w, int)->Element{
                std::string s=left; std::vector<StyledRun> runs=lr;
                int gap=std::max(1,w-string_width(s)-string_width(loc));
                runs.push_back({s.size(),(size_t)gap,Style{}}); s.append((size_t)gap,' ');
                runs.push_back({s.size(),loc.size(),Style{}.with_fg(ext?xc:lc)}); s+=loc;
                return Element{TextElement{ .content=std::move(s), .style=Style{},
                                            .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
            },
            .measure=[](int mw)->Size{ return Size{Columns(mw),Rows(1)}; },
        }};
    }
};

} // namespace maya
