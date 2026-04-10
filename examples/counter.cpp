// counter.cpp — minimal Program example
//
// Demonstrates the type-theoretic architecture:
//   - Model is plain data (no signals, no shared state)
//   - update() is a pure function: (Model, Msg) → (Model, Cmd)
//   - view() is a pure function: Model → Element (using the DSL)
//   - subscribe() maps keys to messages declaratively
//   - Effects are data (Cmd), never performed directly
//   - Cmd<Msg>{} is shorthand for Cmd<Msg>::none() (no effects)

#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;

struct Counter {
    // ── Model: plain value type ───────────────────────────────────────────
    struct Model { int count = 0; };

    // ── Msg: closed sum type (every possible event) ──────────────────────
    struct Increment {};
    struct Decrement {};
    struct Reset {};
    struct Quit {};
    using Msg = std::variant<Increment, Decrement, Reset, Quit>;

    // ── init: just a Model (no startup effects needed) ───────────────────
    static Model init() { return {}; }

    // ── update: pure state transition ────────────────────────────────────
    // Returns {new_model, cmd}. Use {} for no effects (Cmd defaults to none).
    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Increment) { return std::pair{Model{m.count + 1}, Cmd<Msg>{}}; },
            [&](Decrement) { return std::pair{Model{m.count - 1}, Cmd<Msg>{}}; },
            [&](Reset)     { return std::pair{Model{0}, Cmd<Msg>{}}; },
            [](Quit)       { return std::pair{Model{}, Cmd<Msg>::quit()}; },
        }, msg);
    }

    // ── view: pure rendering with the DSL ────────────────────────────────
    static Element view(const Model& m) {
        return v(
            t<"Counter"> | Bold | Fg<100, 180, 255>,
            blank_,
            text(m.count) | Bold,
            blank_,
            t<"+/- to change, r to reset, q to quit"> | Dim
        ) | pad<1> | border_<Round>;
    }

    // ── subscribe: declarative key→message mapping ───────────────────────
    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({
            {'q', Quit{}},
            {'+', Increment{}},  {'=', Increment{}},
            {'-', Decrement{}},  {'r', Reset{}},
            {SpecialKey::Up,   Increment{}},
            {SpecialKey::Down, Decrement{}},
        });
    }
};

// Compile-time proof: Counter satisfies Program.
static_assert(Program<Counter>, "Counter must satisfy the Program concept");

int main() {
    run<Counter>({.title = "counter"});
}
