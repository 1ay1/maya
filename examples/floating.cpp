// examples/floating.cpp — the caret-anchored floating overlay system.
//
//   cmake --build build --target maya_floating && ./build/maya_floating
//
// A block caret you move with the arrow keys; a bordered popup floats next to
// it, flipping above when near the bottom and clamping at the screen edges.
// Space cycles the side (Below/Above/Right/Left). q quits.

#include <maya/maya.hpp>
#include <maya/app/floating.hpp>

#include <string>

using namespace maya;
using namespace maya::dsl;

struct App {
    struct Model { int cx = 4; int cy = 2; int side = 0; };
    struct Move { int dx, dy; }; struct Cycle {}; struct Quit {};
    using Msg = std::variant<Move, Cycle, Quit>;

    static Model init() { return {}; }
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg)) return {m, Cmd<Msg>::quit()};
        if (auto* mv = std::get_if<Move>(&msg)) { m.cx = std::max(0, m.cx + mv->dx); m.cy = std::max(0, m.cy + mv->dy); }
        if (std::holds_alternative<Cycle>(msg)) m.side = (m.side + 1) % 4;
        return {m, Cmd<Msg>::none()};
    }

    static Element view(const Model& m) {
        // background: a caret marker at (cx,cy) via the same absolute-placement helper
        auto caret = maya::detail::place_at(
            text("\xe2\x96\x88") | fgc(Color::hex(0xF9E2AF)), m.cx, m.cy, 1, 1);
        auto bg = zstack({
            text("move the caret with arrows · space cycles side · q quits")
                | fgc(Color::hex(0x585B70)),
            caret,
        });

        // the floating popup
        Element popup = maya::detail::box()
            .border(BorderStyle::Round).border_color(Color::hex(0x89B4FA)).padding(0, 1, 0, 1)
            (v(text("float") | fgc(Color::hex(0xF5F5F7)) | Bold,
               text("side=" + side_name(m.side)) | fgc(Color::hex(0x9399B2)),
               text("(" + std::to_string(m.cx) + "," + std::to_string(m.cy) + ")") | fgc(Color::hex(0x585B70))));

        const Float::Side sides[] = {Float::Side::Below, Float::Side::Above, Float::Side::Right, Float::Side::Left};
        return with_float(bg | grow(1),
            { .content = popup, .cx = m.cx, .cy = m.cy, .w = 20, .h = 5,
              .side = sides[m.side], .gap = 0, .flip = true, .clamp = true });
    }

    static Sub<Msg> subscribe(const Model&) {
        return key_map<Msg>({
            {SpecialKey::Left,  Move{-1, 0}}, {SpecialKey::Right, Move{1, 0}},
            {SpecialKey::Up,    Move{0, -1}}, {SpecialKey::Down,  Move{0, 1}},
            {' ', Cycle{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
        });
    }

    static std::string side_name(int s) {
        const char* n[] = {"Below", "Above", "Right", "Left"};
        return n[s % 4];
    }
};

int main() { run<App>({.title = "maya floating overlays"}); }
