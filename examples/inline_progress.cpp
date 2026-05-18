// inline_progress.cpp — maya::print and maya::live (inline rendering)
//
// Both APIs render INLINE: no alt-screen, no raw mode. Output stays in
// the terminal's normal scrollback like any other program. The example
// is the canonical exercise of the locked-down LiveState chain:
//
//   * maya::print  — one-shot. Internally builds a LiveState, runs one
//                    render_live(...) move-chain step, consumes the
//                    state via std::move(state).finalize(buf), drops it.
//
//   * maya::live   — looped. The state is threaded across frames by
//                    move; user code never sees it, but every iteration
//                    consumes the previous LiveState and produces the
//                    next. There is no path by which a frame can be
//                    rendered against a half-mutated state.
//
// Run:  ./inline_progress           (animates a 3-second progress bar,
//                                    then prints a final summary card)

#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>

#include <chrono>
#include <format>

using namespace maya;
using namespace maya::dsl;

namespace {

constexpr int kBarWidth = 40;
constexpr std::chrono::milliseconds kDuration{3000};

Element progress_card(float fraction, std::chrono::milliseconds elapsed) {
    fraction = std::clamp(fraction, 0.f, 1.f);
    int filled = static_cast<int>(fraction * kBarWidth + 0.5f);

    std::string bar;
    bar.reserve(static_cast<size_t>(kBarWidth) * 3);
    for (int i = 0; i < kBarWidth; ++i) bar += (i < filled) ? "█" : "░";

    auto pct = std::format("{:>3}%", static_cast<int>(fraction * 100));
    auto secs = std::format("{:.1f}s", elapsed.count() / 1000.0);

    return v(
        t<"Working"> | Bold | Fg<140, 200, 255>,
        blank_,
        h(
            text(bar) | Fg<100, 220, 160>,
            text(" "),
            text(pct) | Bold,
            text("   "),
            text(secs) | Dim
        )
    ) | pad<1> | border_<Round>;
}

} // namespace

int main() {
    // ── maya::live — animated, inline ─────────────────────────────────
    // The lambda is called once per frame and returns the Element tree.
    // The LiveState lives inside live<>() and is chained by move; if
    // anything in the witness chain tried to escape this scope, the
    // [[nodiscard]] / move-only / private-field guarantees would surface
    // it as a compile error, not a runtime ghost row.
    auto start = std::chrono::steady_clock::now();

    live({.fps = 30}, [&]() -> Element {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (elapsed >= kDuration) {
            quit();
            return progress_card(1.0f, kDuration);
        }
        float frac = static_cast<float>(elapsed.count()) /
                     static_cast<float>(kDuration.count());
        return progress_card(frac, elapsed);
    });

    // ── maya::print — one-shot, inline ─────────────────────────────────
    // After live() returns, the progress card is already in scrollback.
    // print() appends a styled summary card below it. Same Witness Chain
    // internally, single render_live(...) move step, then finalize.
    print(
        v(
            t<"Done"> | Bold | Fg<100, 255, 140>,
            blank_,
            text("All work completed successfully.") | Dim
        ) | pad<1> | border_<Round>
    );
}
