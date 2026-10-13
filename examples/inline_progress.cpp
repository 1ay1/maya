// examples/inline_progress.cpp — an inline progress card, as a jaal program.
//
// An animated inline widget is an ordinary program run with Mode::Inline:
//
//   Model      how far along the work is (and when it started)
//   Tick       the clock, from Sub::every: advance, and finish at 100%
//   view()     the card, a pure function of the model
//
// Inline mode keeps it in the terminal's normal scrollback, so when the
// program exits the final card stays on screen, and print() adds the
// summary below it.
//
#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int  kBarWidth = 40;
constexpr auto kDuration = 3000ms;

Element progress_card(float fraction, std::chrono::milliseconds elapsed) {
    fraction = std::clamp(fraction, 0.f, 1.f);
    const int filled = static_cast<int>(fraction * kBarWidth + 0.5f);
    std::string bar;
    for (int i = 0; i < kBarWidth; ++i) bar += (i < filled) ? "█" : "░";
    return v(
        t<"Working"> | Bold | Fg<140, 200, 255>,
        blank_,
        h(text(bar) | Fg<100, 220, 160>, text(" "),
          text(std::format("{:>3}%", static_cast<int>(fraction * 100))) | Bold, text("   "),
          text(std::format("{:.1f}s", elapsed.count() / 1000.0)) | Dim)
    ) | pad<1> | border_<Round>;
}

struct Progress {
    struct Model {
        std::chrono::milliseconds elapsed{0};
        std::chrono::milliseconds step{33};
    };
    struct Tick {};
    using Msg = std::variant<Tick>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Tick) {
        m.elapsed = std::min<std::chrono::milliseconds>(m.elapsed + m.step, kDuration);
        // Done: exit after this frame is drawn. The card at 100% stays in
        // the scrollback (inline mode never clears it).
        return m.elapsed >= kDuration ? Cmd::quit(0) : Cmd{};
    }
    static Element view(const Model& m) {
        return progress_card(static_cast<float>(m.elapsed.count()) / kDuration.count(), m.elapsed);
    }
    static Sub subscribe(const Model& m) { return Sub::every(m.step, Tick{}); }
    static bool subs_key(const Model&) { return true; }
};

}  // namespace

int main() {
    const int rc = run<Progress>({.mode = Mode::Inline});
    print(v(t<"Done"> | Bold | Fg<100, 255, 140>, blank_,
            text("All work completed successfully.") | Dim) | pad<1> | border_<Round>);
    return rc;
}
