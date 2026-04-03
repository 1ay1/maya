// maya demo - A complete showcase of the maya TUI library
//
// Demonstrates:
//   1. Nested box layout with borders
//   2. Styled text (bold, colors, dim)
//   3. A reactive counter with Signal
//   4. Keyboard navigation
//   5. Theme colors
//   6. The declarative DSL

#include <maya/maya.hpp>

#include <print>
#include <string>

using namespace maya;

struct Demo {
    Signal<int> counter{0};
    Signal<std::string> message{"Press +/- to change, q to quit"};

    auto render(const Theme& t) -> Element {
        return box().direction(Column).border(BorderStyle::Round).border_color(t.primary).padding(1)(
            // Title
            text(" maya demo ", Style{}.with_bold().with_fg(t.primary)),

            // Blank line
            text(""),

            // Counter row
            box().direction(Row).gap(2)(
                text("Counter: ", Style{}.with_fg(t.muted)),
                text(std::to_string(counter.get()), Style{}.with_bold().with_fg(
                    counter.get() >= 0 ? t.success : t.error
                ))
            ),

            // Blank line
            text(""),

            // Status message
            text(message.get(), Style{}.with_dim().with_fg(t.muted)),

            // Push the help bar to the bottom
            spacer(),

            // Help bar
            box().direction(Row).justify(Justify::SpaceBetween)(
                text("[+/-] count  [r] reset  [q] quit", Style{}.with_dim())
            )
        );
    }
};

int main() {
    auto result = App::builder()
        .title("maya demo")
        .build();

    if (!result) {
        std::println(stderr, "Error: {}", result.error().message);
        return 1;
    }

    auto app = std::move(*result);
    Demo demo;

    app.on_key([&](const KeyEvent& ev) -> bool {
        auto key = ev.key;
        if (auto* ch = std::get_if<CharKey>(&key)) {
            switch (ch->codepoint) {
                case '+': case '=':
                    demo.counter.update([](int& n) { ++n; });
                    return true;
                case '-': case '_':
                    demo.counter.update([](int& n) { --n; });
                    return true;
                case 'r':
                    demo.counter.set(0);
                    demo.message.set("Counter reset!");
                    return true;
                case 'q':
                    app.quit();
                    return true;
                default:
                    break;
            }
        }
        return false;
    });

    auto status = app.run([&] { return demo.render(app.theme()); });
    if (!status) {
        std::println(stderr, "Error: {}", status.error().message);
        return 1;
    }
    return 0;
}
