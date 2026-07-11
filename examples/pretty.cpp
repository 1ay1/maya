// pretty.cpp — the pretty + responsive toolkit, live
//
// Resize your terminal while this runs. Everything re-solves:
//   - gradient() / rainbow()   multi-color text via per-codepoint StyledRuns
//   - gradient_rule()          a full-width divider that tracks the pane width
//   - place()                  pin content to any corner/edge/center of a slot
//   - responsive()             switch whole layouts by width breakpoint
//   - fit_row()                a header that sheds low-priority chips when tight
//
// Not one hand-computed breakpoint or byte count in the file.

#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;

namespace {

constexpr auto kA = Color::hex(0xFF5F6D);   // sunset red
constexpr auto kB = Color::hex(0xFFC371);   // warm gold
constexpr auto kC = Color::hex(0x7F5AF0);   // violet
constexpr auto kD = Color::hex(0x2CB67D);   // green

Element chip(std::string label, Color c) {
    return text(" " + std::move(label) + " ", Style{}.with_fg(c).with_bold());
}

Element hero(int w) {
    // Width-tiered hero: the responsive() tiers pick a treatment by the REAL
    // slot width; inside each tier the gradients just span whatever is there.
    return v(
        gradient("m a y a", kA, kB, Style{}.with_bold()),
        gradient(w >= 72 ? "pretty and responsive, no matter what"
                         : "pretty + responsive",
                 Gradient{{kC, kD, kB}}),
        rainbow("resize me — everything re-solves")
    ) | gap(0);
}

} // namespace

struct Pretty {
    struct Model {};
    struct Quit {};
    using Msg = std::variant<Quit>;

    static Model init() { return {}; }

    static auto update(Model, Msg) -> std::pair<Model, Cmd<Msg>> {
        return {Model{}, Cmd<Msg>::quit()};
    }

    static Element view(const Model&) {
        // Header sheds chips right-to-left as the terminal narrows.
        auto header = fit_row({
            {gradient("PRETTY", kA, kB, Style{}.with_bold())},   // essential
            {Element{space}},
            {chip("gradient", kA), 4},
            {chip("place", kB), 3},
            {chip("responsive", kC), 2},
            {chip("fit_row", kD), 1},                            // first to go
        });

        // Whole-layout breakpoints: one column narrow, two columns wide.
        auto body = responsive({
            {0, [](int w) {
                return Element{v(hero(w)) | padding(1, 2)};
            }},
            {64, [](int w) {
                return Element{h(
                    v(hero(w / 2)) | padding(1, 2) | grow(1),
                    v(place(t<"dead center"> | Bold))
                        | border(BorderStyle::Round) | bcolor(kC) | grow(1)
                ) | gap(1)};
            }},
        });

        return v(
            header | padding(0, 1),
            gradient_rule(kA, kB),
            body | grow(1),
            gradient_rule(kD, kC),
            h(
                place(text("q quits", Style{}.with_dim()), HAlign::Left),
                place(rainbow("♥ maya"), HAlign::Right)
            ) | padding(0, 1) | height(1)
        );
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({{'q', Quit{}}});
    }
};

static_assert(Program<Pretty>, "Pretty must satisfy the Program concept");

int main() {
    run<Pretty>({.title = "pretty"});
}
