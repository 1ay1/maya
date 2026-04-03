// maya demo — full showcase of the declarative API

#include <maya/maya.hpp>
#include <string>

using namespace maya;

int main() {
    Signal<int>         counter{0};
    Signal<std::string> message{"Press +/- to change, q to quit"};

    run(
        {.title = "maya demo"},

        [&](const Event& ev) {
            on(ev, '+', '=', [&] {
                counter.update([](int& n) { ++n; });
                message.set("Counter: " + std::to_string(counter.get()));
            });
            on(ev, '-', '_', [&] {
                counter.update([](int& n) { --n; });
                message.set("Counter: " + std::to_string(counter.get()));
            });
            on(ev, 'r', [&] {
                counter.set(0);
                message.set("Reset!");
            });
            return !key(ev, 'q');
        },

        [&](const Ctx& ctx) {
            return vstack().border(BorderStyle::Round).border_color(ctx.theme.primary).padding(1)(
                text(" maya demo ") | bold | fg(ctx.theme.primary),
                blank(),
                hstack().gap(2)(
                    text("Counter:") | fg(ctx.theme.muted),
                    text(counter.get()) | bold | fg(counter.get() >= 0
                        ? ctx.theme.success : ctx.theme.error)
                ),
                blank(),
                text(message.get()) | dim | fg(ctx.theme.muted),
                spacer(),
                text("[+/-] count  [r] reset  [q] quit") | dim
            );
        }
    );
}
