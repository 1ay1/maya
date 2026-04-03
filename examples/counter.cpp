// Minimal maya counter — the simplest possible maya app.

#include <maya/maya.hpp>

using namespace maya;

int main() {
    Signal<int> count{0};

    run(
        [&](const Event& ev) {
            on(ev, '+', '=', [&] { count.update([](int& n) { ++n; }); });
            on(ev, '-', '_', [&] { count.update([](int& n) { --n; }); });
            return !key(ev, 'q');
        },
        [&] {
            return vstack().padding(1)(
                text("Counter: " + std::to_string(count.get())) | bold,
                text("[+/-] change  [q] quit") | dim
            );
        }
    );
}
