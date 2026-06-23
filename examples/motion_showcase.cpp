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
// Keys:  SPACE toggle the slider · 1/2/3/4/5/6/7 fire the demos
//        c cycle the pulsing colour · q quit
//
//   cmake --build build-test --target maya_motion_showcase
//   ./build-test/maya_motion_showcase

#include <maya/maya.hpp>
#include <maya/anim/text_reveal.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

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

        // 6. Orbiting dots (perpetual motion via pulse + trig)
        bool orbits_on = true;

        // 7. Progress bar with eased fill
        anim::Motion<double> progress{0.0, 0.6};
        int progress_target = 0;

        // 8. Bouncing ball (spring with overshoot)
        anim::Motion<double> ball = anim::Motion<double>::spring(
            0.0, anim::spring_presets::wobbly);
        bool ball_up = false;

        // 9. Wave text (per-char phase offset)
        anim::Mount wave_mount;
        bool wave_playing = false;

        // 10. Matrix rain column
        anim::Mount matrix_mount;
        bool matrix_playing = false;
    };

    struct Toggle {};
    struct FireIntro {};
    struct FireStagger {};
    struct FireReveal {};
    struct CycleColor {};
    struct FireProgress {};
    struct FireBall {};
    struct FireWave {};
    struct FireMatrix {};
    struct Quit {};
    using Msg = std::variant<Toggle, FireIntro, FireStagger, FireReveal,
                             CycleColor, FireProgress, FireBall, FireWave,
                             FireMatrix, Quit>;

    static Model init() { return {}; }

    static Color palette(int i) {
        static const Color stops[] = {
            Color::rgb(120, 200, 255),  // sky
            Color::rgb(255, 120, 200),  // magenta
            Color::rgb(140, 255, 170),  // mint
            Color::rgb(255, 200, 120),  // amber
            Color::rgb(200, 140, 255),  // violet
            Color::rgb(255, 100, 100),  // coral
        };
        return stops[((i % 6) + 6) % 6];
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
            [&](FireProgress) {
                m.progress_target = (m.progress_target + 25) % 125;
                if (m.progress_target > 100) m.progress_target = 0;
                m.progress.to(m.progress_target / 100.0, 0.8,
                              anim::ease::out_back);
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](FireBall) {
                m.ball_up = !m.ball_up;
                m.ball.spring_to(m.ball_up ? 1.0 : 0.0);
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](FireWave) {
                m.wave_mount.remount();
                m.wave_playing = true;
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](FireMatrix) {
                m.matrix_mount.remount();
                m.matrix_playing = true;
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
        constexpr int kBarW = 24;
        const double pos = m.slider.get();                 // ticks + auto-RAF
        const int fill = static_cast<int>(pos * kBarW + 0.5);
        std::string bar;
        for (int i = 0; i < kBarW; ++i)
            bar += (i < fill) ? "\xe2\x96\x88" : "\xe2\x96\x91";  // █ / ░
        auto slider_row = h(
            text("spring slider ", Style{}.with_dim()),
            text(bar, Style{}.with_fg(palette(0))),
            text(" [space]", Style{}.with_dim())
        );

        // ── 2. Eased colour Motion ────────────────────────────────────────
        const Color tint = m.tint.get();                   // ticks + auto-RAF
        auto color_row = h(
            text("color tween   ", Style{}.with_dim()),
            text("\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
                 Style{}.with_fg(tint)),
            text(" [c] cycle", Style{}.with_dim())
        );

        // ── 3. pulse() perpetual breathing ────────────────────────────────
        const double breath = anim::pulse(1400.0);         // 0..1, auto-RAF
        const Color breath_col = anim::lerp(
            Color::rgb(60, 60, 80), Color::rgb(180, 220, 255), breath);
        auto pulse_row = h(
            text("pulse breath  ", Style{}.with_dim()),
            text("\xe2\x97\x8f", Style{}.with_fg(breath_col).with_bold()),
            text(" perpetual", Style{}.with_fg(breath_col))
        );

        // ── 4. Orbiting dots (trig on pulse) ──────────────────────────────
        const double orbit_phase = anim::pulse(2000.0) * 2.0 * 3.14159265;
        const char* orbit_dots[] = {"\xe2\x97\x8f", "\xe2\x97\x8b", "\xe2\x9c\xa6"};  // ● ○ ✦
        // Each dot orbits at a phase offset; sin maps to a horizontal slot,
        // cos drives a depth-brightness illusion (front = bright, back = dim).
        std::vector<Element> orbit_parts;
        orbit_parts.push_back(text("orbiting dots ", Style{}.with_dim()));
        // Render onto a fixed-width track so the dots glide across it.
        constexpr int kOrbitW = 19;
        std::vector<std::pair<int, Element>> placed;
        for (int i = 0; i < 3; ++i) {
            double angle = orbit_phase + i * (2.0 * 3.14159265 / 3.0);
            int x = (kOrbitW / 2) + static_cast<int>(std::sin(angle) * (kOrbitW / 2 - 1));
            double brightness = (std::cos(angle) + 1.0) / 2.0;  // depth illusion
            Color c = anim::lerp(Color::rgb(40, 40, 60), palette(i),
                                 static_cast<float>(brightness));
            placed.push_back({x, text(orbit_dots[i],
                              Style{}.with_fg(c).with_bold())});
        }
        // Lay them out left-to-right by slot, padding the gaps.
        std::sort(placed.begin(), placed.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });
        int cursor = 0;
        for (auto& [x, el] : placed) {
            int gap = x - cursor;
            if (gap > 0) orbit_parts.push_back(text(std::string(gap, ' ')));
            orbit_parts.push_back(std::move(el));
            cursor = x + 1;
        }
        auto orbit_row = h(std::move(orbit_parts));

        // ── 5. Progress bar with expo ease ────────────────────────────────
        constexpr int kProgW = 20;
        const double prog = m.progress.get();
        const int prog_fill = static_cast<int>(prog * kProgW + 0.5);
        std::string prog_bar;
        for (int i = 0; i < kProgW; ++i) {
            if (i < prog_fill) prog_bar += "\xe2\x96\x93";      // ▓
            else if (i == prog_fill) prog_bar += "\xe2\x96\x92"; // ▒
            else prog_bar += "\xe2\x96\x91";                     // ░
        }
        int pct = static_cast<int>(prog * 100 + 0.5);
        auto progress_row = h(
            text("progress bar  ", Style{}.with_dim()),
            text(prog_bar, Style{}.with_fg(palette(2))),
            text(" " + std::to_string(pct) + "% [4]", Style{}.with_dim())
        );

        // ── 6. Bouncing ball ──────────────────────────────────────────────
        constexpr int kBallW = 20;
        const double ball_pos = m.ball.get();
        const int ball_x = static_cast<int>(ball_pos * (kBallW - 1) + 0.5);
        std::string ball_track;
        for (int i = 0; i < kBallW; ++i) {
            if (i == ball_x) ball_track += "\xe2\x97\x8f";  // ●
            else ball_track += "\xc2\xb7";                   // ·
        }
        auto ball_row = h(
            text("bouncy spring ", Style{}.with_dim()),
            text(ball_track, Style{}.with_fg(palette(3))),
            text(" [5]", Style{}.with_dim())
        );

        // ── 7. Wave text (sine offset per char) ───────────────────────────
        const char* wave_str = "WAVE TEXT";
        std::vector<Element> wave_chars;
        wave_chars.push_back(text("wave text     ", Style{}.with_dim()));
        double wave_t = m.wave_playing
            ? static_cast<double>(m.wave_mount.elapsed_ms(3000)) / 1000.0
            : 0.0;
        for (int i = 0; wave_str[i]; ++i) {
            double phase = wave_t * 6.0 - i * 0.5;
            double y_off = std::sin(phase) * 0.5 + 0.5;  // 0..1
            // Color cycles through palette based on position + time
            Color c = anim::lerp(palette(4), palette(1),
                                 static_cast<float>(y_off));
            // We can't actually offset Y in inline, but we can show with
            // brightness + color cycling
            Style st = Style{}.with_fg(c);
            if (y_off > 0.7) st = st.with_bold();
            if (y_off < 0.3) st = st.with_dim();
            wave_chars.push_back(text(std::string(1, wave_str[i]), st));
        }
        wave_chars.push_back(text(" [6]", Style{}.with_dim()));
        auto wave_row = h(std::move(wave_chars));

        // ── 8. Matrix rain ────────────────────────────────────────────────
        constexpr int kMatrixW = 24;
        std::vector<Element> matrix_chars;
        matrix_chars.push_back(text("matrix rain   ", Style{}.with_dim()));
        double mat_t = m.matrix_playing
            ? static_cast<double>(m.matrix_mount.elapsed_ms(4000)) / 1000.0
            : 0.0;
        static const char* glyphs[] = {
            "ア", "イ", "ウ", "エ", "オ", "カ", "キ", "0", "1", "7", "9",
            "\xe2\x96\x88", "\xe2\x96\x93", "\xe2\x96\x92"
        };
        constexpr int kGlyphN = sizeof(glyphs) / sizeof(glyphs[0]);
        for (int i = 0; i < kMatrixW; ++i) {
            // Each column has its own phase
            double col_phase = mat_t * 3.0 - i * 0.15;
            double brightness = std::fmod(col_phase + 100.0, 1.0);
            // Hash for glyph selection
            int glyph_idx = static_cast<int>((i * 7 + static_cast<int>(mat_t * 10)) % kGlyphN);
            Color c;
            if (brightness > 0.9) {
                c = Color::rgb(220, 255, 220);  // white-hot tip
            } else if (brightness > 0.5) {
                c = Color::rgb(0, static_cast<int>(255 * brightness), 0);
            } else {
                c = Color::rgb(0, static_cast<int>(80 * brightness), 0);
            }
            Style st = Style{}.with_fg(c);
            if (brightness > 0.85) st = st.with_bold();
            if (brightness < 0.3) st = st.with_dim();
            matrix_chars.push_back(text(glyphs[glyph_idx], st));
        }
        matrix_chars.push_back(text(" [7]", Style{}.with_dim()));
        auto matrix_row = h(std::move(matrix_chars));

        // ── 9. Timeline (2 parallel tracks: fade + slide) ─────────────────
        if (!m.intro_built) {
            auto op = m.intro.track(0.0);
            op.hold(0.0, 0.10).to(1.0, 0.45, anim::ease::out_cubic);
            auto slide = m.intro.track(20.0);
            slide.at(0.10).to(0.0, 0.50, anim::ease::out_back);  // overshoot pop
            m.intro_built = true;
            m.intro.play();                                // run once on mount
        }
        m.intro.sample();                                  // advance + auto-RAF
        const double tl_op    = m.intro.track_at(0).value();  // fade 0→1
        const double tl_slide = m.intro.track_at(1).value();  // x-offset 20→0
        const int    indent   = static_cast<int>(tl_slide + 0.5);
        auto intro_row = h(
            text("timeline fade ", Style{}.with_dim()),
            text(std::string(indent > 0 ? indent : 0, ' ')),
            text("the quick brown fox",
                 tl_op > 0.5 ? Style{}.with_fg(palette(2)).with_bold()
                             : Style{}.with_fg(palette(2)).with_dim()),
            text(" [1]", Style{}.with_dim())
        );

        // ── 10. Stagger cascade ───────────────────────────────────────────
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
            h(text("stagger list  ", Style{}.with_dim()),
              text("[2] cascade", Style{}.with_dim())),
            v(std::move(cascade))
        );

        // ── 11. Streaming reveal via the framework decorator ──────────────
        static const std::string reveal_body =
            "Text materialises with scramble, gradient trail, and ghost body.";
        TextElement leaf;
        leaf.content = reveal_body;
        if (m.reveal_playing) {
            constexpr double kCpPerMs = 0.06;             // ~60 cp/sec
            const std::int64_t age = m.reveal_mount.elapsed_ms(4000);
            const std::size_t total_cp = string_width(reveal_body);
            const std::size_t revealed =
                std::min<std::size_t>(total_cp,
                    static_cast<std::size_t>(age * kCpPerMs));
            anim::TextRevealParams rp;
            rp.ms_total    = age;
            const std::int64_t arrived_ms =
                static_cast<std::int64_t>(revealed / kCpPerMs);
            rp.edge_age_ms = age - arrived_ms;
            rp.revealed_cp = revealed;
            rp.total_cp    = total_cp;
            anim::decorate_text_reveal(leaf, rp);
            const bool caught_up = revealed >= total_cp;
            const bool cooled    = rp.edge_age_ms > 700;
            if (caught_up && !cooled)
                anim::decorate_end_caret(leaf, rp.ms_total, 650);
        }
        auto reveal_block = v(
            h(text("typewriter    ", Style{}.with_dim()),
              text("[3] reveal", Style{}.with_dim())),
            h(text("  "), Element{std::move(leaf)})
        );

        // ── Spinner row (multiple styles) ─────────────────────────────────
        const double spin_t = anim::pulse(800.0);
        const int spin_frame = static_cast<int>(spin_t * 10) % 10;
        static const char* braille[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
        static const char* spin_dots[] = {"⠁","⠂","⠄","⡀","⢀","⠠","⠐","⠈"};
        static const char* spin_bar[] = {"|","/","-","\\","|","/","-","\\"};
        auto spinner_row = h(
            text("spinners      ", Style{}.with_dim()),
            text(braille[spin_frame % 10], Style{}.with_fg(palette(0)).with_bold()),
            text("  "),
            text(spin_dots[spin_frame % 8], Style{}.with_fg(palette(1)).with_bold()),
            text("  "),
            text(spin_bar[spin_frame % 8], Style{}.with_fg(palette(2)).with_bold()),
            text("  perpetual", Style{}.with_dim())
        );

        return v(
            t<"maya animation framework"> | Bold | Fg<140, 200, 255>,
            t<"no clock · no dt · no RAF anywhere in this file"> | Dim,
            blank_,
            slider_row,
            color_row,
            pulse_row,
            orbit_row,
            spinner_row,
            progress_row,
            ball_row,
            wave_row,
            matrix_row,
            intro_row,
            blank_,
            stagger_block,
            blank_,
            reveal_block,
            blank_,
            t<"q quit"> | Dim
        ) | pad<1> | border_<Round> | bcolor(Color::rgb(80, 90, 110));
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({
            {' ', Toggle{}},
            {'1', FireIntro{}},
            {'2', FireStagger{}},
            {'3', FireReveal{}},
            {'c', CycleColor{}},
            {'4', FireProgress{}},
            {'5', FireBall{}},
            {'6', FireWave{}},
            {'7', FireMatrix{}},
            {'q', Quit{}},
        });
    }
};

static_assert(Program<Showcase>);

int main() {
    run<Showcase>({.title = "maya motion showcase", .mode = Mode::Inline});
}
