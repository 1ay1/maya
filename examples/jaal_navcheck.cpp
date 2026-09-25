// examples/jaal_navcheck.cpp — a tiny list, for the navigation-frame test.
//
// Shows "row N" for the selected row. tests/jaal_nav_frames_test.py sends a
// whole burst of Down arrows in ONE write (what a fast terminal does with
// key repeat) and requires every row to have been drawn, not just the last.
// Built only with -DMAYA_WITH_JAAL=ON.
#include <maya/jaal/host.hpp>
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
    using Sub = jaal::Sub<Msg, on_key>;
    static Cmd update(Model& m, Down) { ++m.row; return {}; }
    static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
    static Element view(const Model& m) { return text("row " + std::to_string(m.row)); }
    static Sub subscribe(const Model&) {
        return jaal_key_map<Sub>({{SpecialKey::Down, Down{}}, {'q', Quit{}}});
    }
};

int main() { return run_jaal<Nav>({.title = "nav"}); }
