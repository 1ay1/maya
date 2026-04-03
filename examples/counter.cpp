// Minimal maya counter example
//
// The simplest possible maya app: a reactive counter with keyboard controls.

#include <maya/maya.hpp>

#include <print>
#include <string>

using namespace maya;

int main() {
    auto result = App::builder().title("counter").build();
    if (!result) {
        std::println(stderr, "Error: {}", result.error().message);
        return 1;
    }

    auto app = std::move(*result);
    Signal<int> count{0};

    app.on_key([&](const KeyEvent& ev) -> bool {
        if (auto* ch = std::get_if<CharKey>(&ev.key)) {
            if (ch->codepoint == '+' || ch->codepoint == '=') { count.update([](int& n) { ++n; }); return true; }
            if (ch->codepoint == '-' || ch->codepoint == '_') { count.update([](int& n) { --n; }); return true; }
            if (ch->codepoint == 'q') { app.quit(); return true; }
        }
        return false;
    });

    auto status = app.run([&] {
        return box().direction(Column).padding(1)(
            text("Counter: " + std::to_string(count.get()), bold_style),
            text("[+/-] change  [q] quit", dim_style)
        );
    });

    if (!status) { std::println(stderr, "Error: {}", status.error().message); return 1; }
    return 0;
}
