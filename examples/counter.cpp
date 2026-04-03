// Minimal maya counter — the simplest possible maya app.
// Demonstrates: Signal, run(), key() predicates, element DSL.

#include <maya/maya.hpp>
#include <string>

using namespace maya;

int main() {
    Signal<int> count{0};

    run(
        [&](const Event& ev) {
            if (key(ev, '+') || key(ev, '=')) count.update([](int& n) { ++n; });
            if (key(ev, '-') || key(ev, '_')) count.update([](int& n) { --n; });
            return !key(ev, 'q');
        },
        [&] {
            return box().direction(Column).padding(1)(
                text("Counter: " + std::to_string(count.get()), bold_style),
                text("[+/-] change  [q] quit", dim_style)
            );
        }
    );
}
