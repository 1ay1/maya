// adaptive.cpp — pick() + clamp() + fit_col(): good at EVERY size, both axes
//
// Three primitives, one idea — measure, don't estimate:
//
//   pick({rich, medium, tiny})   the first alternative that FITS its slot
//                                (SwiftUI's ViewThatFits) — no breakpoints,
//                                the decision comes from measuring the real
//                                styled fragments
//   clamp(content, 100)          content stops growing at 100 cells and
//                                centers (libadwaita's AdwClamp) — ultrawide
//                                terminals stop stretching your UI thin
//   fit_col({{el, keep}, …})     the vertical fit_row: sheds low-priority
//                                panels when the terminal is SHORT, instead
//                                of shearing borders mid-panel
//
// Resize the terminal in both directions:
//   narrow    → the header swaps to shorter alternatives, word by word
//   ultrawide → the dashboard stays 100 wide, centered
//   short     → DISK drops first, then NET, then MEM — CPU never drops
//
// q quits.

#include <maya/maya.hpp>

#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

namespace {

Element meter(std::string label, float frac) {
    return adapt([label = std::move(label), frac](int w) -> Element {
        const int bar_w = std::max(4, w - 12);
        const int on = static_cast<int>(frac * static_cast<float>(bar_w) + 0.5f);
        std::string bar;
        for (int i = 0; i < bar_w; ++i) bar += (i < on) ? "█" : "░";
        return h(
            text(label, Style{}.with_dim()) | width(6),
            text(bar),
            text(" " + std::to_string(static_cast<int>(frac * 100)) + "%") | width(5)
        ).build();
    });
}

Element panel(std::string title, std::vector<Element> body) {
    auto b = vstack();
    b.border(BorderStyle::Round);
    b.border_text(" " + std::move(title) + " ", BorderTextPos::Top);
    b.padding(0, 1, 0, 1);
    return b(std::move(body));
}

Element cpu()  { return panel("CPU",  {meter("core0", 0.72f), meter("core1", 0.31f)}); }
Element mem()  { return panel("MEM",  {meter("used",  0.63f), meter("cache", 0.22f)}); }
Element net()  { return panel("NET",  {meter("rx",    0.41f), meter("tx",    0.08f)}); }
Element disk() { return panel("DISK", {meter("read",  0.17f), meter("write", 0.54f)}); }

} // namespace

struct Adaptive {
    struct Model {};
    struct Quit {};
    using Msg = std::variant<Quit>;

    static Model init() { return {}; }
    static auto update(Model, Msg) -> std::pair<Model, Cmd<Msg>> {
        return {Model{}, Cmd<Msg>::quit()};
    }

    static Element view(const Model&) {
        // The richest header that actually fits — measured, not breakpointed.
        auto header = pick({
            h(text("◈ adaptive", Style{}.with_bold()),
              text("  ·  host ayu-linux01  ·  kernel 7.1.3-zen  ·  up 15d 4h  ·  387 procs")),
            h(text("◈ adaptive", Style{}.with_bold()), text("  ·  ayu-linux01")),
            text("◈ ad", Style{}.with_bold()),
        });

        // Panels shed lowest-keep-first when the terminal is short.
        auto panels = fit_col({
            {cpu(), kKeepAlways},
            {mem(), 4},
            {net(), 2},
            {disk(), 1},
        });

        // The dashboard never grows past 100 cells; centered beyond that.
        return v(
            header | padding(0, 1),
            sep,
            clamp(panels, 100) | grow(1)
        );
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({{'q', Quit{}}});
    }
};

static_assert(Program<Adaptive>, "Adaptive must satisfy the Program concept");

int main() {
    run<Adaptive>({.title = "adaptive"});
}
