#pragma once
// maya::widget::WelcomeScreen — empty-thread brand splash + orientation.
//
// Quiet brand presence at the top of an empty conversation:
//
//   • A chunky 6×7 PIXEL-ART WORDMARK reading "» AGENTTY" — drawn into
//     a half-block canvas, not the terminal font. The leading double-
//     chevron reads as "this command is executing."
//   • Three layered animations:
//       — Phase 1 [0, ~1.3 s]: CASCADE DROP. Each glyph starts off-
//         screen above the canvas and falls into place with a 100 ms
//         stagger and ease-out cubic — like the letters are being
//         dealt onto the screen one at a time.
//       — Phase 2 forever after: PER-LETTER WAVE. Every glyph
//         independently bobs vertically with a sine offset that is
//         phase-shifted by the letter's index, so the motion reads
//         as a slow sine wave traveling left → right through the
//         wordmark, continuously. ±1.5 px amplitude, 2.2 s period.
//       — Heartbeat pulse: every ~3.2 s the entire wordmark flashes
//         bright_white for 80 ms — a system-alive blink layered on
//         top of the wave.
//
//   • A tagline (italic, dim).
//   • A center chip row with model + profile chip.
//   • An optional bordered "Try" starter card.
//   • A bottom hint row of keyboard shortcuts.
//
// The animation clock lives entirely inside `build()` (a function-local
// static). The widget detects "I just became visible again" by noting
// the wall-clock gap since the previous build call — a long gap (default
// 500 ms) means the widget was off-screen, and the animation restarts.
// Once the spiral is complete, every frame renders the same Element tree
// and maya's diff collapses to a no-op (zero terminal write, zero
// perpetual CPU).
//
//   maya::WelcomeScreen{{
//       .sigil_color    = Color::magenta(),
//       .tagline        = "a calm middleware between you and the model",
//       .model_badge    = ModelBadge{"claude-opus-4-7"}.build(),
//       .profile_label  = "write",
//       .profile_color  = Color::magenta(),
//       .hints          = {{"^K", " palette", Color::cyan()},
//                          {"^J", " threads", Color::cyan()}},
//   }}.build();

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../app/app.hpp"
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class WelcomeScreen {
public:
    struct Hint {
        std::string key;        // e.g. "^K"
        std::string label;      // e.g. " palette" — caller controls leading space
        Color       key_color = Color::cyan();
    };

    struct Config {
        // Brand mark color. The sigil's body uses this; the freshest 2-3
        // dots at the spiral's drawing tip flash bright_white, and old
        // dots fade to bright_black — giving a comet-tail look without
        // leaving the named-ANSI palette.
        Color                    sigil_color = Color::magenta();

        std::string              tagline;

        // Center chip row: caller-built model badge sits beside a profile chip
        Element                  model_badge;       // default-empty
        std::string              profile_label;   // raw — widget renders verbatim
        Color                    profile_color = Color::magenta();

        // Starters card
        std::string              starters_title = "Try";
        std::vector<std::string> starters;

        // Bottom hint row
        std::string              hint_intro = "type to begin";
        std::vector<Hint>        hints;

        Color                    accent_color = Color::magenta();
        Color                    text_color   = Color::bright_white();

        // Sigil intro: total draw-in time in ms. Set to 0 to render the
        // completed spiral statically from frame 1 (skip the intro).
        int                      sigil_draw_ms = 1800;

        // Row CAP for the whole widget (0 = uncapped). The widget now
        // sheds against its REAL allocated height — the layout is built
        // inside a component whose render sees the definite slot, so in
        // fullscreen the budget measures itself and this knob can stay
        // 0. Hosts running INLINE mode should still set it to
        // terminal_rows minus their surrounding chrome: an auto-height
        // root gives the welcome as many rows as it wants, so the slot
        // cannot tell the widget the terminal is shorter — and a
        // welcome that overflows the viewport is a scrollback hazard
        // (the overflow rows commit to native scrollback at startup,
        // and the first welcome→conversation swap strands them there
        // permanently).
        int                      max_rows = 0;
    };

    explicit WelcomeScreen(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Height- AND width-aware: the whole layout is built inside a
        // component so the shedding budget below reads the REAL
        // allocated (w, h) at render time — the definite fullscreen
        // slot measures itself; cfg.max_rows remains as the host CAP
        // for inline/auto-height roots where the slot is as tall as the
        // content wants (the layout cannot know the terminal there and
        // an overflowing welcome is a scrollback hazard).
        return detail::component([self = *this](int w, int h) -> Element {
            return self.build_at(w, h);
        });
    }

private:
    [[nodiscard]] Element build_at(int slot_w, int slot_h) const {
        using namespace dsl;

        const Color muted = Color::bright_black();

        auto centered = [](Element e) {
            return h(spacer(), std::move(e), spacer()).build();
        };
        auto centered_text = [&](std::string s, Style st) {
            return centered(text(std::move(s), st));
        };

        const int age_ms = static_cast<int>(animation_age_ms_());
        // The sigil has two phases — a one-shot draw-in then a perpetual
        // concentric ripple — both rely on per-frame redraws. The frame
        // request is owned by anim::Mount::elapsed_ms() (called inside
        // animation_age_ms_) — it keeps the loop awake while the welcome is
        // on screen and lapses to idle the moment build() stops being
        // called (screen dismissed). No hand-rolled request_animation_frame
        // here anymore.

        auto sigil = centered(render_sigil_(age_ms));

        auto tagline = centered_text(cfg_.tagline,
                                     Style{}.with_fg(muted).with_italic());

        // Profile chip: ▌ INVERSE-LABEL ▐
        auto profile_chip = h(
            text("\xe2\x96\x8c", Style{}.with_fg(cfg_.profile_color)),    // ▌
            text(" " + cfg_.profile_label + " ",
                 Style{}.with_fg(cfg_.profile_color).with_inverse().with_bold()),
            text("\xe2\x96\x90", Style{}.with_fg(cfg_.profile_color))     // ▐
        );
        auto chips_row = h(
            spacer(),
            cfg_.model_badge,
            text("    "),
            profile_chip,
            spacer());

        const bool show_starters = !cfg_.starters.empty();

        // ── Viewport clamp ──
        // Row budget for THIS widget: terminal rows minus the host
        // chrome below us. The full layout is 4 content rows (sigil,
        // tagline, chips, hint) + kSigilCh-1 extra sigil rows + 6 blank
        // separators (+ starters card when present). When the budget is
        // tighter we drop blanks in reverse-priority order, then
        // collapse the pixel sigil to a one-row text wordmark. The
        // essential rows (sigil/wordmark, tagline, chips, hint) always
        // render — on an absurdly short terminal the frame may still
        // overflow by a row or two, but that floor is 4 rows.
        constexpr int kSigilCh = (kFontH + 1 + 2 + 1) / 2;  // = render_sigil_'s CH
        // MEASURED budget: the rows the slot actually affords, capped
        // by the host's max_rows when set (inline-mode safety valve).
        const int cap    = cfg_.max_rows > 0 ? cfg_.max_rows : 1 << 20;
        const int budget = std::min(cap, slot_h > 0 ? slot_h : 1 << 20);

        // Essential content rows: tagline + chips + hint. The hint is
        // width-responsive (greedy line fill), so count its REAL line
        // count at the REAL slot width — under-counting it by one is
        // exactly the off-by-one that pushes the frame into scrollback.
        const int hint_rows = hint_line_count_(
            cfg_, std::max(0, (slot_w > 0 ? slot_w : available_width()) - 2));
        const int kEssential = 2 + std::max(1, hint_rows);
        const bool pixel_sigil = budget >= kEssential + kSigilCh;
        const int sigil_h = pixel_sigil ? kSigilCh : 1;
        int spare = budget - (kEssential + sigil_h);
        if (spare < 0) spare = 0;
        if (spare > 6) spare = 6;   // 6 separators = the full layout

        Element sigil_el = pixel_sigil
            ? centered(render_sigil_(age_ms))
            // One-row wordmark fallback — keeps the brand presence on
            // short terminals without the 5-row pixel canvas.
            : centered_text("\xc2\xbb A G E N T T Y",
                            Style{}.with_fg(cfg_.sigil_color).with_bold());

        // Blank separators by priority: the ones that carry the most
        // visual structure go first, so a shrinking budget degrades
        // gracefully (sigil↔tagline gap survives longest).
        //   b0 sigil→tagline   b1 tagline→chips   b2 chips→hint
        //   b3 top pad         b4 2nd tagline→chips  b5 2nd chips→hint
        const bool b0 = spare > 0, b1 = spare > 1, b2 = spare > 2,
                   b3 = spare > 3, b4 = spare > 4, b5 = spare > 5;

        std::vector<Element> rows;
        if (b3) rows.push_back(blank());
        rows.push_back(std::move(sigil_el));
        if (b0) rows.push_back(blank());
        rows.push_back(tagline);
        if (b1) rows.push_back(blank());
        if (b4) rows.push_back(blank());
        rows.push_back(chips_row);
        if (b2) rows.push_back(blank());
        if (b5) rows.push_back(blank());

        // Bottom hint row — responsive. At wide widths the chips lay
        // out on a single centered line with `intro · key label · key
        // label · …`; as the terminal narrows the row wraps into
        // additional centered lines (greedy fit), and below a hard
        // floor the labels collapse to keys-only. The whole thing is
        // a ComponentElement because we need the allocated width to
        // pick the layout — a static h(...) of all parts would let
        // maya's per-cell wrap shred the chips into a column (the
        // `·` separator becoming its own dangling row), which is the
        // failure mode this branch fixes.
        auto hint = Element{ComponentElement{
            .render = [cfg = cfg_, muted](int w, int /*h*/) -> Element {
                return render_hints_(cfg, muted, w);
            },
        }};

        // Starters card only at full budget — it's the most decorative
        // tier and the first to go under the clamp.
        if (show_starters && spare >= 6) {
            std::vector<Element> starter_rows;
            starter_rows.push_back(text(" " + small_caps_(cfg_.starters_title) + " ",
                                        Style{}.with_fg(muted).with_bold()));
            starter_rows.push_back(blank());
            for (const auto& s : cfg_.starters) {
                starter_rows.push_back(h(
                    text("\xe2\x80\xa2 ", fg_dim_(cfg_.accent_color)),     // •
                    text(s, fg_dim_(cfg_.text_color))
                ).build());
            }
            auto starters_card = (v(starter_rows)
                                  | padding(0, 2, 0, 2)
                                  | border(BorderStyle::Round)
                                  | bcolor(muted)).build();
            rows.push_back(h(spacer(), starters_card, spacer()));
            rows.push_back(blank());
            rows.push_back(blank());
        }
        rows.push_back(hint);
        return (v(rows) | padding(0, 1) | grow(1.0f)).build();
    }

    Config cfg_;

    // ─────────────────────────────────────────────────────────────────────
    // Animation clock — owned by the maya animation framework.
    // ─────────────────────────────────────────────────────────────────────
    // Elapsed ms since the welcome screen mounted. anim::Mount owns the
    // remount detection (a >500 ms gap between builds = was off-screen →
    // restart from 0) AND the per-frame request, so this is now a one-liner
    // instead of the hand-rolled steady_clock + statics + gap heuristic.
    // Static Mount because the welcome screen is a process singleton and its
    // intro should restart whenever it reappears, not per-instance.
    static std::int64_t animation_age_ms_() noexcept {
        static anim::Mount mount;
        return mount.elapsed_ms();   // 0 ⇒ perpetual frame requests while shown
    }

    // ──────────────────────────────────────────────────────────────────
    // Responsive hint-row layout.
    // ──────────────────────────────────────────────────────────────────
    // Greedy line-fill: place the `intro · key label · key label …`
    // sequence one chip at a time, opening a new centered row whenever
    // the next chip would overflow the allocated `avail` width. Below
    // a hard floor (kKeysOnlyFloor cols) labels are dropped so the
    // chips degrade to `^K · ^J · ^T …`; if even that doesn't fit,
    // the keys themselves greedily wrap. Width 0 means "layout has
    // not allocated yet" — we fall back to the single-line form so
    // the first frame's render is sensible.
    // Pure row-count mirror of render_hints_'s greedy line fill — the
    // clamp in build() needs the hint's height BEFORE the component's
    // deferred render runs. Must stay in lockstep with render_hints_
    // (same chip widths, same separator width, same wrap rule).
    [[nodiscard]] static int hint_line_count_(const Config& cfg, int avail) {
        constexpr int kKeysOnlyFloor = 40;
        const int sep_w = string_width("  \xc2\xb7  ");
        const bool drop_labels = avail > 0 && avail < kKeysOnlyFloor;

        std::vector<int> widths;
        widths.reserve(cfg.hints.size() + 1);
        if (!cfg.hint_intro.empty())
            widths.push_back(string_width(cfg.hint_intro));
        for (const auto& h_ : cfg.hints)
            widths.push_back(drop_labels
                ? string_width(h_.key)
                : string_width(h_.key) + string_width(h_.label));

        const int max_line = (avail > 0) ? avail : (1 << 30);
        // NOTE: mirrors render_hints_ exactly, including its quirk of
        // adding the pre-wrap `add` (with sep_w) to the fresh line.
        int lines = 1, line_w = 0;
        bool line_empty = true;
        for (int w : widths) {
            const int add = w + (line_empty ? 0 : sep_w);
            if (!line_empty && line_w + add > max_line) {
                ++lines;
                line_w = 0;
                line_empty = true;
            }
            line_w += add;
            line_empty = false;
        }
        return lines;
    }

    [[nodiscard]] static Element render_hints_(
        const Config& cfg, Color muted, int avail)
    {
        using namespace dsl;
        constexpr int kKeysOnlyFloor = 40;
        constexpr const char* kSep = "  \xc2\xb7  ";   // "  ·  "
        const int sep_w = string_width(kSep);
        const bool drop_labels = avail > 0 && avail < kKeysOnlyFloor;

        struct Chip { Element key; Element label; int width; bool has_label; };
        std::vector<Chip> chips;
        chips.reserve(cfg.hints.size() + 1);

        // Treat the intro ("type to begin") as a labelless chip so it
        // participates in the same wrap logic — at very narrow widths
        // it'd otherwise force the first row to be wider than the rest.
        if (!cfg.hint_intro.empty()) {
            chips.push_back(Chip{
                .key       = text(cfg.hint_intro, fg_dim_(muted)),
                .label     = text(""),
                .width     = string_width(cfg.hint_intro),
                .has_label = false,
            });
        }
        for (const auto& h_ : cfg.hints) {
            Element key = text(h_.key,
                               Style{}.with_fg(h_.key_color).with_bold());
            if (drop_labels) {
                chips.push_back(Chip{
                    .key       = std::move(key),
                    .label     = text(""),
                    .width     = string_width(h_.key),
                    .has_label = false,
                });
            } else {
                chips.push_back(Chip{
                    .key       = std::move(key),
                    .label     = text(h_.label, fg_dim_(muted)),
                    .width     = string_width(h_.key) + string_width(h_.label),
                    .has_label = true,
                });
            }
        }

        // Single-line fallback when width is unknown (first frame) or
        // generously wide — also the happy path most users see.
        const int max_line = (avail > 0) ? avail : (1 << 30);

        std::vector<std::vector<int>> lines;   // chip indices per line
        lines.emplace_back();
        int line_w = 0;
        for (int i = 0; i < static_cast<int>(chips.size()); ++i) {
            const int add = chips[static_cast<std::size_t>(i)].width
                          + (lines.back().empty() ? 0 : sep_w);
            if (!lines.back().empty() && line_w + add > max_line) {
                lines.emplace_back();
                line_w = 0;
            }
            lines.back().push_back(i);
            line_w += add;
        }

        std::vector<Element> rendered_lines;
        rendered_lines.reserve(lines.size());
        for (const auto& line : lines) {
            std::vector<Element> parts;
            parts.reserve(line.size() * 4 + 2);
            parts.push_back(spacer());
            for (std::size_t k = 0; k < line.size(); ++k) {
                if (k != 0) parts.push_back(text(kSep, fg_dim_(muted)));
                const auto& c = chips[static_cast<std::size_t>(line[k])];
                parts.push_back(c.key);
                if (c.has_label) parts.push_back(c.label);
            }
            parts.push_back(spacer());
            rendered_lines.push_back(h(std::move(parts)).build());
        }
        return v(std::move(rendered_lines)).build();
    }

    // ─────────────────────────────────────────────────────────────────────
    // The wordmark — "agentty" drawn as a custom 5×8 pixel-art bitmap
    // font, rendered through half-block glyphs (▀ / ▄ / █). Each
    // terminal cell holds two vertical pixels; unset pixels emit a
    // plain space so the terminal background shows through cleanly.
    //
    // Animation:
    //   Phase 1 [0, sigil_draw_ms]:  one-shot left→right wipe. The
    //     letters appear as a bright_white "shimmer band" sweeps
    //     across the canvas; pixels behind the band lock in at
    //     sigil_color, pixels ahead are not yet drawn.
    //   Phase 2 [sigil_draw_ms, ∞]:  perpetual sinusoidal shimmer. The
    //     wordmark is fully drawn in sigil_color; the shimmer band
    //     keeps sliding back and forth across it (sine timing for
    //     natural deceleration at the endpoints), highlighting
    //     whichever pixels it's currently passing through.
    // ─────────────────────────────────────────────────────────────────────

    // 6×7 pixel-art font. Chunky uppercase, no serifs. '#' = lit,
    // ' ' = empty. The leading '»' glyph adds an "executing command"
    // beat at the front of the wordmark.
    static constexpr int kFontW = 6;
    static constexpr int kFontH = 7;

    struct Glyph { const char* rows[kFontH]; };

    [[nodiscard]] static const Glyph& glyph_for_(char c) noexcept {
        static constexpr Glyph CHEV{{
            "      ",
            "#  #  ",
            "## ## ",
            " ## ##",
            "## ## ",
            "#  #  ",
            "      ",
        }};
        static constexpr Glyph A{{
            "  ##  ",
            " #  # ",
            "#    #",
            "######",
            "#    #",
            "#    #",
            "#    #",
        }};
        static constexpr Glyph G{{
            " #### ",
            "#    #",
            "#     ",
            "#  ###",
            "#    #",
            "#    #",
            " #### ",
        }};
        static constexpr Glyph E{{
            "######",
            "#     ",
            "#     ",
            "##### ",
            "#     ",
            "#     ",
            "######",
        }};
        static constexpr Glyph N{{
            "#    #",
            "##   #",
            "# #  #",
            "#  # #",
            "#   ##",
            "#    #",
            "#    #",
        }};
        static constexpr Glyph T{{
            "######",
            "  ##  ",
            "  ##  ",
            "  ##  ",
            "  ##  ",
            "  ##  ",
            "  ##  ",
        }};
        static constexpr Glyph Y{{
            "#    #",
            "#    #",
            " #  # ",
            "  ##  ",
            "  ##  ",
            "  ##  ",
            "  ##  ",
        }};
        static constexpr Glyph BLANK{{
            "      ", "      ", "      ", "      ",
            "      ", "      ", "      ",
        }};
        switch (c) {
            case 'a': case 'A': return A;
            case 'g': case 'G': return G;
            case 'e': case 'E': return E;
            case 'n': case 'N': return N;
            case 't': case 'T': return T;
            case 'y': case 'Y': return Y;
            case '>':           return CHEV;
            default:            return BLANK;
        }
    }

    [[nodiscard]] Element render_sigil_(int age_ms) const {
        using namespace dsl;

        // Layout: chevron + AGENTTY, 1-pixel spacer between glyphs.
        static constexpr std::string_view kText  = ">AGENTTY";
        static constexpr int               kSpacer = 1;
        constexpr int PW = static_cast<int>(kText.size()) * kFontW
                         + (static_cast<int>(kText.size()) - 1) * kSpacer;

        // Canvas vertical layout: a 1-px pad on top + the kFontH font
        // body + a 2-px pad on bottom (enough room for the ±1.5 px
        // bob in Phase 2 plus the heartbeat flash to look uncramped).
        constexpr int kPadTop     = 1;
        constexpr int kPadBottom  = 2;
        constexpr int kLetterY    = kPadTop;
        constexpr int PH          = kFontH + kPadTop + kPadBottom;
        constexpr int CH          = (PH + 1) / 2;
        constexpr int kGridRows   = CH * 2;

        // ── Animation parameters ────────────────────────────────────
        // Cascade drop
        constexpr int   kStaggerMs       = 100;
        constexpr int   kDropDurationMs  = 500;
        const     int   kPhase1EndMs     = static_cast<int>(kText.size() - 1)
                                         * kStaggerMs + kDropDurationMs;
        // Per-letter bob (Phase 2)
        constexpr float kBobAmp          = 1.5f;        // px
        constexpr float kBobPeriodMs     = 2200.0f;
        constexpr float kBobLetterPhase  = 0.7f;        // rad / letter
        // Heartbeat pulse
        constexpr int   kPulsePeriod     = 3200;
        constexpr int   kPulseWidth      = 80;

        // Heartbeat — only fires after Phase 1 settles so the cascade
        // entry doesn't get strobed.
        const bool in_pulse = age_ms > kPhase1EndMs
            && ((age_ms - kPhase1EndMs) % kPulsePeriod) < kPulseWidth;

        // Per-letter Y offset. Combines cascade drop (Phase 1) and
        // sine bob (Phase 2). Negative = above letter's home row.
        auto letter_y_offset = [&](int li) -> float {
            const int drop_start = li * kStaggerMs;
            const int drop_end   = drop_start + kDropDurationMs;
            // Off-screen-above starting position — full kFontH + the
            // top pad, so the bottom-row pixels are also above the
            // canvas before the drop animates.
            const float kOff = -static_cast<float>(kFontH + kPadTop + 1);
            if (age_ms < drop_start) return kOff;
            if (age_ms < drop_end) {
                const float t = static_cast<float>(age_ms - drop_start)
                              / static_cast<float>(kDropDurationMs);
                const float eased = 1.0f
                                  - (1.0f - t) * (1.0f - t) * (1.0f - t);
                return kOff * (1.0f - eased);
            }
            // Phase 2 — per-letter sine bob with phase offset.
            const float t = static_cast<float>(age_ms - drop_end);
            const float phase = 2.0f * 3.14159265f * t / kBobPeriodMs
                              + static_cast<float>(li) * kBobLetterPhase;
            return std::sin(phase) * kBobAmp;
        };

        // Pixel grid — taller than kFontH to give the bob room and
        // pad an even number of rows for the half-block cell loop.
        std::vector<std::optional<Color>> px(
            static_cast<std::size_t>(PW * kGridRows));

        const Color pixel_color = in_pulse
            ? Color::bright_white()
            : cfg_.sigil_color;

        for (std::size_t li = 0; li < kText.size(); ++li) {
            const Glyph& g      = glyph_for_(kText[li]);
            const int    base_x = static_cast<int>(li) * (kFontW + kSpacer);
            // Snap to integer pixel offset — terminal pixels are
            // integer-grid, anti-aliased bob would just create flicker.
            const int    dy     = static_cast<int>(
                std::lround(letter_y_offset(static_cast<int>(li))));
            for (int row = 0; row < kFontH; ++row) {
                const char* row_str = g.rows[row];
                for (int col = 0; col < kFontW; ++col) {
                    if (row_str[col] != '#') continue;
                    const int x = base_x + col;
                    const int y = kLetterY + row + dy;
                    if (y < 0 || y >= kGridRows) continue;
                    px[static_cast<std::size_t>(y)
                       * static_cast<std::size_t>(PW)
                       + static_cast<std::size_t>(x)] = pixel_color;
                }
            }
        }

        // Render each cell row as a horizontal sequence of per-cell
        // half-block elements. 24×6 = 144 cells total — well below
        // the threshold where Element-count overhead matters.
        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(CH));
        for (int cy = 0; cy < CH; ++cy) {
            std::vector<Element> cells;
            cells.reserve(static_cast<std::size_t>(PW));
            for (int x = 0; x < PW; ++x) {
                const auto top = px[static_cast<std::size_t>((cy * 2) * PW + x)];
                const auto bot = px[static_cast<std::size_t>((cy * 2 + 1) * PW + x)];
                cells.push_back(half_block_cell_(top, bot));
            }
            rows.push_back(h(std::move(cells)).build());
        }
        return v(std::move(rows)).build();
    }

    // Compose one cell from optional top/bottom pixel colors. Empty top &
    // bottom → plain space (cell takes the terminal background, no
    // black silhouette). Single-side → ▀ or ▄ with fg only (bg stays
    // default). Both sides same color → █. Mixed → ▀ with fg=top,
    // bg=bottom — the standard half-block trick.
    static Element half_block_cell_(std::optional<Color> top,
                                     std::optional<Color> bot)
    {
        using namespace dsl;
        if (!top && !bot)               return text(" ");
        if (top && !bot)                return text("\xe2\x96\x80",
                                                    Style{}.with_fg(*top));
        if (!top && bot)                return text("\xe2\x96\x84",
                                                    Style{}.with_fg(*bot));
        if (top && bot && color_eq_(*top, *bot))
                                        return text("\xe2\x96\x88",
                                                    Style{}.with_fg(*top));
        return text("\xe2\x96\x80",
                    Style{}.with_fg(*top).with_bg(*bot));
    }

    // Coarse equality on Color: enough to spot "both halves are the same
    // shade so render █ instead of ▀ with redundant bg" — saves SGR
    // bytes on the diff stream and reads cleaner in screenshots.
    static bool color_eq_(const Color& a, const Color& b) noexcept {
        if (a.kind() != b.kind()) return false;
        switch (a.kind()) {
            case Color::Kind::Default: return true;
            case Color::Kind::Named:
            case Color::Kind::Indexed: return a.index() == b.index();
            case Color::Kind::Rgb:
                return a.r() == b.r() && a.g() == b.g() && a.b() == b.b();
        }
        return false;
    }

    static std::string small_caps_(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            out.push_back(static_cast<char>(
                (c >= 'a' && c <= 'z') ? (c - 32) : c));
            if (i + 1 < s.size()) out.push_back(' ');
        }
        return out;
    }

    static Style fg_dim_(Color c) {
        const bool is_already_muted =
            c.kind() == Color::Kind::Named
            && c.index() == static_cast<uint8_t>(AnsiColor::BrightBlack);
        return is_already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
