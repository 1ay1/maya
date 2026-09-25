// jaal_basic.cpp — maya's counter.cpp, on jaal.
//
// A line-for-line port of examples/counter.cpp, so the two can be read side
// by side. What changes is exactly the program shape:
//
//   maya                                   jaal
//   static Model init()                    (gone: Model is value-initialised)
//   update(Model m, Msg) -> pair<M, Cmd>   update(Model& m, Case) -> Cmd,
//     + a std::visit over every case         one overload per case
//   return {Model{m.count+1}, Cmd{}}       ++m.count; return {};
//   Sub<Msg> + key_map<Msg>(...)           jaal::Sub + jaal_key_map<Sub>(...)
//   run<Counter>(...)                      run_jaal<Counter>(...)
//
// view() is unchanged: the toolkit is the same toolkit.

#include <maya/jaal/host.hpp>
#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;

struct Counter {
    struct Model { int count = 0; };

    struct Increment {};
    struct Decrement {};
    struct Reset {};
    struct Quit {};
    using Msg = std::variant<Increment, Decrement, Reset, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Increment) { ++m.count; return {}; }
    static Cmd update(Model& m, Decrement) { --m.count; return {}; }
    static Cmd update(Model& m, Reset)     { m.count = 0; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return v(
            t<"Counter"> | Bold | Fg<100, 180, 255>,
            blank_,
            text(m.count) | Bold,
            blank_,
            t<"+/- to change, r to reset, q to quit"> | Dim
        ) | pad<1> | border_<Round>;
    }

    static Sub subscribe(const Model&) {
        return jaal_key_map<Sub>({
            {'q', Quit{}},
            {'+', Increment{}},  {'=', Increment{}},
            {'-', Decrement{}},  {'r', Reset{}},
            {SpecialKey::Up,   Increment{}},
            {SpecialKey::Down, Decrement{}},
        });
    }
};

static_assert(JaalView<Counter>);

int main() {
    return run_jaal<Counter>({.title = "counter"});
}
