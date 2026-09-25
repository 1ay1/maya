// examples/counter.cpp — maya running on jaal.
//
// The first maya program on the new runtime. Same toolkit, same terminal
// handling, same rendering — only the loop underneath is jaal's.
//
//   +/-      change the count
//   space    start / stop a ticking timer (a Sub that comes and goes)
//   q, ^C    quit
//

#include <maya/app.hpp>
#include <maya/maya.hpp>

#include <chrono>
#include <string>
#include <variant>

using namespace std::chrono_literals;
using namespace maya;
using namespace maya::dsl;

struct Counter {
    struct Model {
        int  count   = 0;
        int  ticks   = 0;
        bool ticking = false;
        int  width   = 0;
        int  height  = 0;
    };

    struct Inc {};
    struct Dec {};
    struct Toggle {};
    struct Tick {};
    struct Resized { int w, h; };
    struct Quit {};
    using Msg = std::variant<Inc, Dec, Toggle, Tick, Resized, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_resize, jaal::fx::on_signal>;

    static Cmd update(Model& m, Inc)       { ++m.count; return {}; }
    static Cmd update(Model& m, Dec)       { --m.count; return {}; }
    static Cmd update(Model& m, Toggle)    { m.ticking = !m.ticking; return {}; }
    static Cmd update(Model& m, Tick)      { ++m.ticks; return {}; }
    static Cmd update(Model& m, Resized r) { m.width = r.w; m.height = r.h; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return v(
            t<"maya on jaal"> | Bold | Fg<100, 180, 255>,
            text(""),
            text("count: " + std::to_string(m.count)) | Bold,
            text(std::string("timer: ") + (m.ticking ? "on" : "off")
                 + "   ticks: " + std::to_string(m.ticks)),
            text("terminal: " + std::to_string(m.width) + "x" + std::to_string(m.height)),
            text(""),
            t<"+/- count   space timer   q quit"> | Dim
        ) | padding(1) | border(BorderStyle::Round);
    }

    // What subscribe() reads: only `ticking` decides the timer. The key and
    // resize routers capture nothing, so they can't go stale.
    static bool subs_key(const Model& m) { return m.ticking; }

    static Sub subscribe(const Model& m) {
        auto keys = Sub::on(on_key{}, [](const KeyEvent& e) -> std::optional<Msg> {
            if (auto* c = std::get_if<CharKey>(&e.key)) {
                switch (c->codepoint) {
                    case U'+': case U'=': return Inc{};
                    case U'-':            return Dec{};
                    case U' ':            return Toggle{};
                    case U'q':            return Quit{};
                    default:              return std::nullopt;
                }
            }
            return std::nullopt;
        });
        auto resize = Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
            return Resized{r.width.raw(), r.height.raw()};
        });
        auto sigint = Sub::on_signal({jaal::sig::interrupt},
                                     [](jaal::sig) -> std::optional<Msg> { return Quit{}; });
        if (m.ticking)
            return Sub::batch(std::move(keys), std::move(resize), std::move(sigint),
                              Sub::every(250ms, Tick{}));
        return Sub::batch(std::move(keys), std::move(resize), std::move(sigint));
    }
};

int main() {
    return run<Counter>({.title = "maya on jaal"});
}
