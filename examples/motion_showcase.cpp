// motion_showcase.cpp — the maya animation FRAMEWORK in one screen
// ============================================================================
//
// Everything here is animated, and NOWHERE in this file do we read a clock,
// compute a dt, or call request_animation_frame(). That is the whole point:
// the maya animation framework (core/motion.hpp + anim/text_reveal.hpp) owns
// time, ticking, frame cadence, and lifecycle. A widget author just declares
// intent and reads a value.
//
//   • Motion<T>     a self-driving value. set a target, read .get() in view().
//   • pulse()       perpetual breathing phase off wall-clock.
//   • Timeline      multi-step choreography (keyframes / holds / parallel).
//   • Stagger       index-phased fan-out (the cascade-in list).
//   • text_reveal   the streaming typewriter as a one-call decorator.
//
// Keys:  SPACE toggle the slider · 1/2/3 fire the timeline / stagger / reveal
//        c cycle the pulsing colour · q quit
//
//   cmake --build build-test --target maya_motion_showcase
//   ./build-test/maya_motion_showcase

#include <maya/maya.hpp>
#include <maya/anim/text_reveal.hpp>

#include <string>

using namespace maya;
using namespace maya::dsl;

struct Showcase {
    struct Model {
        // 1. A self-driving slider position (0..1). Spring mode = momentum.
        anim::Motion<double> slider = anim::Motion<double>::spring(
            0.0, anim::spring_presets::wobbly);
        bool slider_on = false;

        // 2. A colour that eases between palette stops on demand.
        anim::Motion<Color> tint{Color::rgb(120, 200, 255), 0.4};
        int tint_idx = 0;

        // 3. A multi-step intro timeline (replayable). Built lazily on first
        //    view so the track schedule is defined once.
        anim::Timeline intro;
        bool intro_built = false;

        // 4. A stagger clock for the cascade-in list.
        anim::Mount stagger_mount;
        bool        stagger_playing = false;

        // 5. The streaming-reveal demo: a Mount-driven typewriter over a
        //    fixed body, using the framework reveal decorator directly.
        anim::Mount reveal_mount;
        bool        reveal_playing = false;
    };

    struct Toggle {};
    struct FireIntro {};
    struct FireStagger {};
    struct FireReveal {};
    struct CycleColor {};
    struct Quit {};
    using Msg = std::variant<Toggle, FireIntro, FireStagger, FireReveal,
                             CycleColor, Quit>;

    static Model init() { return {}; }

    static Color palette(int i) {
        static const Color stops[] = {
            Color::rgb(120, 200, 255),  // sky
            Color::rgb(255, 120, 200),  // magenta
            Color::rgb(140, 255, 170),  // mint
            Color::rgb(255, 200, 120),  // amber
        };
        return stops[((i % 4) + 4) % 4];
    }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Toggle) {
                m.slider_on = !m.slider_on;
                m.slider.spring_to(m.slider_on ? 1.0 : 0.0);
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](FireIntro) {
                m.intro.play();
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](FireStagger) {
                m.stagger_mount.remount();
                m.stagger_playing = true;
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](FireReveal) {
                m.reveal_mount.remount();
                m.reveal_playing = true;
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](CycleColor) {
                m.tint_idx++;
                m.tint.to(palette(m.tint_idx), 0.4);
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [](Quit) { return std::pair{Model{}, Cmd<Msg>::quit()}; },
        }, msg);
    }

    // ── view: pure, declarative, animated ────────────────────────────────
    static Element view(const Model& m_const) {
        // The animation objects tick in view() (they're mutable-friendly);
        // cast away const just for the read since Model is handed to us const.
        Model& m = const_cast<Model&>(m_const);

        // ── 1. Spring slider ──────────────────────────────────────────────
        constexpr int kBarW = 32;
        const double pos = m.slider.get();                 // ticks + auto-RAF
        const int fill = static_cast<int>(pos * kBarW + 0.5);
        std::string bar;
        for (int i = 0; i < kBarW; ++i)
            bar += (i < fill) ? "\xe2\x96\x88" : "\xe2\x96\x91";  // █ / ░
        auto slider_row = h(
            text("spring  ", Style{}.with_dim()),
            text(bar, Style{}.with_fg(palette(0))),
            text("  [space]", Style{}.with_dim())
        );

        // ── 2. Eased colour Motion ────────────────────────────────────────
        const Color tint = m.tint.get();                   // ticks + auto-RAF
        auto color_row = h(
            text("tint    ", Style{}.with_dim()),
            text("\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
                 Style{}.with_fg(tint)),
            text("  [c] cycle", Style{}.with_dim())
        );

        // ── 3. pulse() perpetual breathing ────────────────────────────────
        const double breath = anim::pulse(1400.0);         // 0..1, auto-RAF
        const Color breath_col = anim::lerp(
            Color::rgb(60, 60, 80), Color::rgb(180, 220, 255), breath);
        auto pulse_row = h(
            text("pulse   ", Style{}.with_dim()),
            text("\xe2\x97\x8f breathing", Style{}.with_fg(breath_col).with_bold())
        );

        // ── 4. Timeline (2 parallel tracks: fade + slide) ─────────────────
        if (!m.intro_built) {
            auto op = m.intro.track(0.0);
            op.hold(0.0, 0.10).to(1.0, 0.45, anim::ease::out_cubic);
            auto slide = m.intro.track(20.0);
            slide.at(0.10).to(0.0, 0.50, anim::ease::out_back);  // overshoot pop
            m.intro_built = true;
        }
        m.intro.sample();                                  // advance + auto-RAF
        const double tl_op = m.intro.track_at(0).value();  // read, don't create
        auto intro_row = h(
            text("timeline", Style{}.with_dim()),
            text("  the quick brown fox",
                 tl_op > 0.5 ? Style{}.with_fg(palette(2)).with_bold()
                             : Style{}.with_fg(palette(2)).with_dim()),
            text("  [1] replay", Style{}.with_dim())
        );

        // ── 5. Stagger cascade ────────────────────────────────────────────
        std::vector<Element> cascade;
        const double s_elapsed =
            m.stagger_playing
                ? static_cast<double>(m.stagger_mount.elapsed_ms(700)) / 1000.0
                : 1.0;  // settled
        static const char* items[] = {"deploy", "verify", "promote", "done"};
        for (int i = 0; i < 4; ++i) {
            const double p = anim::stagger_progress(s_elapsed, i, 0.08, 0.35);
            Style st = Style{}.with_fg(palette(i));
            if (p < 0.5) st = st.with_dim();
            if (p < 0.05) st = st.with_fg(Color::rgb(40, 40, 50));
            cascade.push_back(text(std::string("  \xe2\x80\xa2 ") + items[i], st));
        }
        auto stagger_block = v(
            h(text("stagger ", Style{}.with_dim()),
              text("[2] cascade in", Style{}.with_dim())),
            v(std::move(cascade))
        );

        // ── 6. Streaming reveal via the framework decorator ───────────────
        static const std::string reveal_body =
            "Streaming text materialises with a scramble tip, a hot to cool "
            "gradient trail, and a ghosted not-yet-typed body.";
        TextElement leaf;
        leaf.content = reveal_body;
        if (m.reveal_playing) {
            const std::int64_t age = m.reveal_mount.elapsed_ms(4000);
            // ~80 cp/sec typewriter via a RateCursor-free fixed mapping.
            const std::size_t total_cp = string_width(reveal_body);
            const std::size_t revealed =
                std::min<std::size_t>(total_cp,
                    static_cast<std::size_t>(age * 0.06));
            anim::TextRevealParams rp;
            rp.ms_total    = m.reveal_mount.elapsed_ms(4000);
            rp.edge_age_ms = 0;            // freshest at the cursor
            rp.revealed_cp = revealed;
            rp.total_cp    = total_cp;
            anim::decorate_text_reveal(leaf, rp);
        }
        auto reveal_block = v(
            h(text("reveal  ", Style{}.with_dim()),
              text("[3] type it out", Style{}.with_dim())),
            h(text("  "), Element{std::move(leaf)})
        );

        return v(
            t<"maya animation framework"> | Bold | Fg<140, 200, 255>,
            t<"no clock · no dt · no request_animation_frame in this file"> | Dim,
            blank_,
            slider_row,
            color_row,
            pulse_row,
            intro_row,
            blank_,
            stagger_block,
            blank_,
            reveal_block,
            blank_,
            t<"q to quit"> | Dim
        ) | pad<1> | border_<Round> | bcolor(Color::rgb(80, 90, 110));
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({
            {' ', Toggle{}},
            {'1', FireIntro{}},
            {'2', FireStagger{}},
            {'3', FireReveal{}},
            {'c', CycleColor{}},
            {'q', Quit{}},
        });
    }
};

static_assert(Program<Showcase>);

int main() {
    run<Showcase>({.title = "maya motion showcase", .mode = Mode::Inline});
}
