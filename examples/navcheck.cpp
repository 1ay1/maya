// examples/navcheck.cpp — a tiny list, for the navigation-frame test.
//
// Shows "row N" for the selected row. tests/nav_frames_test.py sends a
// whole burst of Down arrows in ONE write (what a fast terminal does with
// key repeat) and requires every row to have been drawn, not just the last.
#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;

struct Nav {
    struct Model { int row = 0; };
    struct Down {}; struct Quit {};
    using Msg = std::variant<Down, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Down) { ++m.row; return {}; }
    static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
    static Element view(const Model& m) { return text("row " + std::to_string(m.row)); }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{SpecialKey::Down, Down{}}, {'q', Quit{}}};
        return keys(k);
    }
};

int main() { return run<Nav>({.title = "nav"}); }
