// examples/counter.cpp — the smallest maya app.
//
// maya is to jaal what Ink is to React: jaal runs the program (model,
// update, effects, subscriptions), maya draws it. This is the README's
// quickstart, verbatim.
//
//   +/-   change the count      q   quit

#include <maya/host/run.hpp>

#include <optional>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;

struct Counter {
    struct Model { int count = 0; };

    struct Inc {}; struct Dec {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Quit>;

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Inc)  { ++m.count; return {}; }
    static Cmd update(Model& m, Dec)  { --m.count; return {}; }
    static Cmd update(Model&,   Quit) { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return v(
            text("Count: " + std::to_string(m.count)) | Bold | Fg<100, 200, 255>,
            t<"[+/-] change  [q] quit"> | Dim
        ) | border_<Round> | bcol<50, 55, 70> | pad<1>;
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}};
        return keys(k);
    }
};

int main() { return run<Counter>({.title = "counter"}); }
