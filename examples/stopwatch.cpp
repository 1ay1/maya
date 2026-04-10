// stopwatch.cpp — Timer-driven Program example
//
// Demonstrates:
//   - Sub::every() for periodic tick subscriptions
//   - Cmd::after() for delayed effects
//   - Conditional subscriptions based on model state
//   - Rich DSL composition with dynamic content
//   - key_map() for declarative key bindings

#include <maya/maya.hpp>
#include <chrono>

using namespace maya;
using namespace maya::dsl;

struct Stopwatch {
    // ── Model ────────────────────────────────────────────────────────────
    struct Model {
        int  centiseconds = 0;  // elapsed time in 1/100s
        bool running      = false;
        int  laps         = 0;
        bool flash        = false;  // brief highlight on lap
    };

    // ── Msg ──────────────────────────────────────────────────────────────
    struct Tick {};
    struct Toggle {};        // start/stop
    struct Lap {};
    struct Reset {};
    struct FlashOff {};      // clear the lap highlight
    struct Quit {};
    using Msg = std::variant<Tick, Toggle, Lap, Reset, FlashOff, Quit>;

    // ── init ─────────────────────────────────────────────────────────────
    static Model init() { return {}; }

    // ── update ───────────────────────────────────────────────────────────
    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Tick) {
                m.centiseconds++;
                return std::pair{m, Cmd<Msg>{}};
            },
            [&](Toggle) {
                m.running = !m.running;
                return std::pair{m, Cmd<Msg>{}};
            },
            [&](Lap) {
                m.laps++;
                m.flash = true;
                // Clear the flash after 300ms
                return std::pair{m, Cmd<Msg>::after(std::chrono::milliseconds(300), FlashOff{})};
            },
            [&](Reset) {
                return std::pair{Model{}, Cmd<Msg>{}};
            },
            [&](FlashOff) {
                m.flash = false;
                return std::pair{m, Cmd<Msg>{}};
            },
            [](Quit) {
                return std::pair{Model{}, Cmd<Msg>::quit()};
            },
        }, msg);
    }

    // ── view ─────────────────────────────────────────────────────────────
    static Element view(const Model& m) {
        int mins = m.centiseconds / 6000;
        int secs = (m.centiseconds / 100) % 60;
        int cs   = m.centiseconds % 100;

        auto time_str = std::format("{:02}:{:02}.{:02}", mins, secs, cs);

        return v(
            t<"Stopwatch"> | Bold | Fg<100, 180, 255>,
            blank_,
            when(m.flash,
                text(time_str) | Bold | Fg<255, 220, 80>,
                text(time_str) | Bold | Fg<100, 255, 180>),
            blank_,
            h(
                when(m.running,
                    text("RUNNING") | Bold | Fg<80, 220, 120>,
                    text("STOPPED") | Dim),
                when(m.laps > 0,
                    text(std::format("  {} laps", m.laps)) | Dim)
            ),
            blank_,
            t<"space start/stop, l lap, r reset, q quit"> | Dim
        ) | pad<1> | border_<Round>;
    }

    // ── subscribe ────────────────────────────────────────────────────────
    // Tick subscription is conditional: only active when running.
    static auto subscribe(const Model& m) -> Sub<Msg> {
        auto keys = key_map<Msg>({
            {'q', Quit{}},
            {' ', Toggle{}},
            {'l', Lap{}},
            {'r', Reset{}},
        });

        if (m.running) {
            // 10ms tick = centisecond precision
            return Sub<Msg>::batch(
                std::move(keys),
                Sub<Msg>::every(std::chrono::milliseconds(10), Tick{})
            );
        }
        return keys;
    }
};

static_assert(Program<Stopwatch>);

int main() {
    run<Stopwatch>({.title = "stopwatch"});
}
