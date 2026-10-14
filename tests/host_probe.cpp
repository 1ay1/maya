// tests/host_probe.cpp — the program tests/host_pty_test.py drives.
//
// Shows count, timer ticks and terminal size so the pty test can read them
// off the screen: keys, a burst of keys, a timer that comes and goes,
// SIGWINCH, and quit.
//
//   +/-  count   space  timer on/off   q  quit

#include <maya/host/run.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <variant>

using namespace std::chrono_literals;
using namespace maya;
using namespace maya::dsl;

struct Probe {
    struct Model {
        int  count   = 0;
        int  ticks   = 0;
        bool ticking = false;
        int  width   = 0;
        int  height  = 0;
    };

    struct Inc {}; struct Dec {}; struct Toggle {}; struct Tick {}; struct Quit {};
    struct Resized { int w, h; };
    using Msg = std::variant<Inc, Dec, Toggle, Tick, Resized, Quit>;

    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg>;
    using Event = std::variant<KeyEvent, ResizeEvent>;

    static Cmd update(Model& m, Inc)       { ++m.count; return {}; }
    static Cmd update(Model& m, Dec)       { --m.count; return {}; }
    static Cmd update(Model& m, Toggle)    { m.ticking = !m.ticking; return {}; }
    static Cmd update(Model& m, Tick)      { ++m.ticks; return {}; }
    static Cmd update(Model& m, Resized r) { m.width = r.w; m.height = r.h; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return v(
            text("count: " + std::to_string(m.count)) | Bold,
            text(std::string("timer: ") + (m.ticking ? "on" : "off")
                 + "   ticks: " + std::to_string(m.ticks)),
            text("terminal: " + std::to_string(m.width) + "x" + std::to_string(m.height)),
            t<"+/- count   space timer   q quit"> | Dim
        ) | padding(1) | border(BorderStyle::Round);
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{
            {'+', Inc{}}, {'=', Inc{}}, {'-', Dec{}}, {' ', Toggle{}}, {'q', Quit{}}};
        return keys(k);
    }
    static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
        return Resized{r.width.value, r.height.value};
    }

    static bool subs_key(const Model& m) { return m.ticking; }
    static Sub subscribe(const Model& m) {
        return m.ticking ? Sub::every(250ms, Tick{}) : Sub::none();
    }
};

static_assert(maya::Program<Probe>);

int main() { return run<Probe>({.title = "host probe"}); }
