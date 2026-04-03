// maya — Compile-time DSL demo
//
// Demonstrates maya's compile-time UI DSL where tree structure, text content,
// styles, and layout properties are all resolved at compile time.
// Type-state machines enforce correctness — impossible states don't compile.

#include <maya/maya.hpp>
#include <string>

using namespace maya;
using namespace maya::dsl;

int main() {
    // ── Static UI — fully resolved at compile time ───────────────────────
    // Tree structure, strings, styles, layout are all template parameters.
    // .build() converts to runtime Element (inlined, zero overhead).

    constexpr auto header = h(
        t<"\xe2\x9a\xa1 maya"> | Bold | Fg<100, 180, 255>,
        t<" \xe2\x80\x94 compile-time DSL"> | Dim
    );

    constexpr auto card = v(
        t<"System Status"> | Bold | Fg<220, 220, 240>,
        t<"">,
        h(
            t<"CPU:">  | Dim,
            t<" 42%">  | Bold | Fg<80, 220, 120>
        ),
        h(
            t<"Mem:">  | Dim,
            t<" 8.2G"> | Bold | Fg<180, 130, 255>
        ),
        h(
            t<"Disk:"> | Dim,
            t<" 67%">  | Bold | Fg<240, 200, 60>
        )
    ) | border_<Round> | bcol<60, 65, 80> | pad<1>;

    // Type-state safety: bcol<...> only compiles after border_<...>.
    // Try removing border_<Round> above — it won't compile!

    constexpr auto status = h(
        t<"\xe2\x97\x8f Online"> | Bold | Fg<80, 220, 120>,
        t<"  \xe2\x97\x8f 3 services"> | Fg<100, 180, 255>,
        t<"  \xe2\x97\x8b 0 alerts"> | Dim
    );

    constexpr auto ui = v(
        header,
        t<"">,
        card,
        t<"">,
        status
    );

    // One call to print — that's it
    print(ui.build());

    // ── Dynamic content — dyn() escapes to runtime ───────────────────────
    int count = 42;
    auto dynamic_ui = v(
        t<"Dynamic content:"> | Bold,
        dyn([&] { return text("  count = " + std::to_string(count), Style{}.with_fg(Color::rgb(100, 255, 180))); }),
        t<"  (dyn() wraps a runtime lambda in the compile-time tree)"> | Dim
    );

    print(text(""));
    print(dynamic_ui.build());
}
