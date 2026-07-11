// grid.cpp — a rockbottom-shaped system dashboard in ONE grid declaration
//
// The Bootstrap idea, for terminal cells: every column states its width PER
// BREAKPOINT and the grid re-solves the packing at every resize. This file
// recreates the layout skeleton of rockbottom (github.com/1ay1/rockbottom) —
// which originally needed a hand-rolled three-tier switch — as:
//
//   grid({
//       col(stats).xxl(Columns{42}),   // fixed 42-cell sidebar on ultrawide
//       col(table),                    // ...the process table takes the rest
//   })
//
// where `stats` is itself a grid whose cells go 2-across at md and restack
// automatically inside the narrow xxl sidebar — because maya breakpoints key
// on the SLOT width, not the screen width. Resize the terminal:
//
//   < 90 cols   everything stacks in one column
//   ≥ 90 cols   stat panels pair up 2×2 above the full-width table
//   ≥ 200 cols  stats become a fixed-width sidebar, table fills the rest
//
// Zero width arithmetic, zero tier switches. q quits.

#include <maya/maya.hpp>

#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

namespace {

const Gradient kHeat{{Color::hex(0x2CB67D), Color::hex(0xF1C40F),
                      Color::hex(0xE74C3C)}};

// A labelled value bar colored by load — the bar re-solves to the REAL cell
// width via adapt(), so it is correct inside any grid cell at any tier.
Element meter(std::string label, float frac) {
    return adapt([label = std::move(label), frac](int w) -> Element {
        const int bar_w = std::max(4, w - 12);
        const int on = static_cast<int>(frac * static_cast<float>(bar_w) + 0.5f);
        std::string bar;
        for (int i = 0; i < bar_w; ++i) bar += (i < on) ? "█" : "░";
        return h(
            text(label, Style{}.with_dim()) | width(6),
            text(bar, Style{}.with_fg(kHeat.at(frac))),
            text(" " + std::to_string(static_cast<int>(frac * 100)) + "%",
                 Style{}.with_bold()) | width(5)
        ).build();
    });
}

Element panel(std::string title, std::vector<Element> body) {
    auto b = vstack();
    b.border(BorderStyle::Round);
    b.border_color(Color::hex(0x44475A));
    b.border_text(" " + std::move(title) + " ", BorderTextPos::Top);
    b.padding(0, 1, 0, 1);
    return b(std::move(body));
}

Element cpu()  { return panel("CPU",  {meter("core0", 0.72f), meter("core1", 0.31f),
                                       meter("core2", 0.55f), meter("core3", 0.12f)}); }
Element mem()  { return panel("MEM",  {meter("used",  0.63f), meter("cache", 0.22f)}); }
Element net()  { return panel("NET",  {meter("rx",    0.41f), meter("tx",    0.08f)}); }
Element disk() { return panel("DISK", {meter("read",  0.17f), meter("write", 0.54f)}); }

Element table() {
    std::vector<Element> rows;
    rows.push_back(h(
        text("PID", Style{}.with_bold()) | width(8),
        text("NAME", Style{}.with_bold()) | grow(1),
        text("CPU%", Style{}.with_bold()) | width(6),
        text("MEM", Style{}.with_bold()) | width(8)
    ));
    struct P { const char* pid; const char* name; const char* c; const char* m; };
    for (auto& p : {P{"1", "systemd", "0.1", "12M"}, P{"842", "kernel_task", "3.2", "480M"},
                    P{"1337", "rb", "1.4", "24M"},   P{"2001", "cc1plus", "97.8", "1.1G"},
                    P{"2002", "cc1plus", "96.2", "1.0G"}, P{"3120", "firefox", "8.4", "2.3G"}})
        rows.push_back(h(
            text(p.pid) | width(8),
            text(p.name) | grow(1),
            text(p.c) | width(6),
            text(p.m) | width(8)
        ));
    return panel("PROCESSES", std::move(rows));
}

} // namespace

struct GridDemo {
    struct Model {};
    struct Quit {};
    using Msg = std::variant<Quit>;

    static Model init() { return {}; }
    static auto update(Model, Msg) -> std::pair<Model, Cmd<Msg>> {
        return {Model{}, Cmd<Msg>::quit()};
    }

    static Element view(const Model&) {
        // The stats block: 2-across at md, restacks by itself inside the
        // 42-cell xxl sidebar (its SLOT drops below the md threshold there).
        auto stats = grid({
            col(cpu()).md(6),
            col(mem()).md(6),
            col(net()).md(6),
            col(disk()).md(6),
        });

        // The whole dashboard: one declaration, three shapes.
        auto body = grid({
            col(stats).xxl(Columns{42}),
            col(table()),
        });

        return v(
            h(gradient("griddemo", Color::hex(0xFF5F6D), Color::hex(0xFFC371),
                       Style{}.with_bold()),
              space,
              text("resize me", Style{}.with_dim())) | padding(0, 1),
            gradient_rule(Color::hex(0x7F5AF0), Color::hex(0x2CB67D)),
            body | grow(1),
            h(place(text("q quits", Style{}.with_dim()), HAlign::Right))
                | padding(0, 1) | height(1)
        );
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({{'q', Quit{}}});
    }
};

static_assert(Program<GridDemo>, "GridDemo must satisfy the Program concept");

int main() {
    run<GridDemo>({.title = "grid"});
}
