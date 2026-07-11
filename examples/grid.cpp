// grid.cpp — a rockbottom-shaped system dashboard in two lines
//
// The whole responsive story:
//
//   auto stats = row({cpu, mem, net, disk});      // side by side, share width
//   auto body  = sidebar(stats, table, 42);       // 42-col rail + main
//
// row() puts cells side by side sharing the width equally — and wraps, then
// stacks, by itself as the slot narrows (grid() is the same engine when you
// want to tune the cell width: grid(cells, 26)). col() stacks cells, each
// stretched to the full width. sidebar() puts a fixed rail beside the main
// pane and stacks the pair when the terminal is too narrow. Because every
// piece re-solves from the width of the slot it SITS IN, the stats restack
// 1-across by themselves inside the 42-cell rail. Resize the terminal:
//
//   wide      stats rail (1-across) beside a big process table
//   medium    stats flow 2-, 3-across over a full-width table
//   narrow    everything in one column
//
// Zero width arithmetic, zero tier switches, zero breakpoints. q quits.

#include <maya/maya.hpp>

#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

namespace {

const Gradient kHeat{{Color::hex(0x2CB67D), Color::hex(0xF1C40F),
                      Color::hex(0xE74C3C)}};

// A labelled value bar colored by load — the bar re-solves to the REAL cell
// width via adapt(), so it is correct inside any grid cell at any width.
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
        // Two lines, three layouts. The stats row restacks by itself
        // inside the 42-cell rail because it keys on its SLOT width.
        auto stats = row({cpu(), mem(), net(), disk()});
        auto body  = sidebar(stats, table(), 42);

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
