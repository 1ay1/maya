// maya inline mode — renders in the terminal scrollback, not alt screen.
//
// Keys: +/- = change count   t = cycle color   q = quit

#include <maya/maya.hpp>

using namespace maya;
using namespace maya::dsl;

int main() {
    int count = 0;
    int theme = 0;

    constexpr int kThemes = 5;
    const Color colors[] = {
        Color::rgb(80, 200, 255), Color::rgb(80, 220, 120),
        Color::rgb(255, 160, 80), Color::rgb(180, 130, 255),
        Color::rgb(255, 100, 100),
    };
    const char* names[] = {"cyan", "green", "orange", "purple", "red"};

    run(
        {.alt_screen = false},

        [&](const Event& ev) {
            on(ev, '+', '=', [&] { ++count; });
            on(ev, '-', '_', [&] { --count; });
            on(ev, 't', [&] { theme = (theme + 1) % kThemes; });
            return !key(ev, 'q');
        },

        [&] {
            auto c = colors[theme];
            auto accent = Style{}.with_bold().with_fg(c);
            auto dim    = Style{}.with_fg(c);

            int filled = std::abs(count) % 21;
            std::string bar;
            for (int i = 0; i < 20; ++i)
                bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";

            return (v(
                text("Count: " + std::to_string(count), accent),
                text(bar, dim),
                blank_,
                text("theme: " + std::string(names[theme]), dim),
                t<"[+/-] count  [t] color  [q] quit"> | Dim
            ) | border_<Round> | pad<0, 1>).build();
        }
    );
}
