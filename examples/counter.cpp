// Minimal maya counter — the simplest possible maya app.

#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;

int main() {
    Signal<int> count{0};

    run(
        [&](const Event& ev) {
            on(ev, '+', '=', [&] { count.update([](int& n) { ++n; }); });
            on(ev, '-', '_', [&] { count.update([](int& n) { --n; }); });
            return !key(ev, 'q');
        },
        [&] {
            return (v(
                dyn([&] { return text("Counter: " + std::to_string(count.get()), Style{}.with_bold()); }),
                t<"[+/-] change  [q] quit"> | Dim
            ) | pad<1>).build();
        }
    );
}
