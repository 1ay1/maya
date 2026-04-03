// maya demo — full showcase rewritten with the new run() interface
//
// Before (old API):   ~40 lines, manual App builder, on_key registration,
//                     std::get_if variant dispatch, explicit error handling
// After  (new API):   ~25 lines, one run() call, readable key() predicates,
//                     zero boilerplate

#include <maya/maya.hpp>
#include <string>

using namespace maya;

int main() {
    Signal<int>         counter{0};
    Signal<std::string> message{"Press +/- to change, q to quit"};

    run(
        {.title = "maya demo"},

        // ── Event handler ─────────────────────────────────────────────────
        // Return false to quit, true to keep running.
        [&](const Event& ev) {
            if (key(ev, '+') || key(ev, '=')) {
                counter.update([](int& n) { ++n; });
                message.set("Counter: " + std::to_string(counter.get()));
            }
            if (key(ev, '-') || key(ev, '_')) {
                counter.update([](int& n) { --n; });
                message.set("Counter: " + std::to_string(counter.get()));
            }
            if (key(ev, 'r')) {
                counter.set(0);
                message.set("Reset!");
            }
            return !key(ev, 'q');
        },

        // ── Render function ───────────────────────────────────────────────
        // Ctx carries the live terminal size and active theme.
        // Rebuild the Element tree every frame — diff handles the rest.
        [&](const Ctx& ctx) {
            auto num_style = Style{}.with_bold().with_fg(
                counter.get() >= 0 ? ctx.theme.success : ctx.theme.error);

            return box().direction(Column)
                        .border(BorderStyle::Round)
                        .border_color(ctx.theme.primary)
                        .padding(1)(
                text(" maya demo ", Style{}.with_bold().with_fg(ctx.theme.primary)),
                text(""),
                box().direction(Row).gap(2)(
                    text("Counter:", Style{}.with_fg(ctx.theme.muted)),
                    text(std::to_string(counter.get()), num_style)
                ),
                text(""),
                text(message.get(), Style{}.with_dim().with_fg(ctx.theme.muted)),
                spacer(),
                text("[+/-] count  [r] reset  [q] quit", dim_style)
            );
        }
    );
}
