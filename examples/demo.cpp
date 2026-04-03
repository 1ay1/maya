// maya demo — full showcase of the declarative API

#include <maya/maya.hpp>
#include <string>

using namespace maya;
using namespace maya::dsl;

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
            // DSL tree with dyn() for runtime content
            return (v(
                dyn([&] { return text(" maya demo ", Style{}.with_bold().with_fg(ctx.theme.primary)); }),
                blank_,
                dyn([&] {
                    auto c = counter.get();
                    return (h(
                        dyn([&] { return text("Counter:", Style{}.with_fg(ctx.theme.muted)); }),
                        dyn([&, c] {
                            return text(std::to_string(c),
                                Style{}.with_bold().with_fg(c >= 0 ? ctx.theme.success : ctx.theme.error));
                        })
                    ) | gap_<2>).build();
                }),
                blank_,
                dyn([&] { return text(message.get(), Style{}.with_dim().with_fg(ctx.theme.muted)); }),
                space,
                t<"[+/-] count  [r] reset  [q] quit"> | Dim
            ) | border_<Round> | pad<1>).build();
        }
    );
}
