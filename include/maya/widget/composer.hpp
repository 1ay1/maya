#pragma once
// maya::widget::Composer — bordered input box reflecting agent state.
//
// State-driven input box: border / prompt / placeholder change with
// activity (idle, awaiting permission, streaming, executing tool); the
// hint row carries Send / newline / expand keys plus right-side ambient
// indicators (queued count, word/token counters, profile chip). Wraps,
// caps height, pins height during activity to prevent jitter, and adds
// a bottom-right "N lines" caption when multi-line.
//
//   maya::Composer{{
//       .text        = m.ui.composer.text,
//       .cursor      = m.ui.composer.cursor,
//       .state       = compute_state(m),
//       .active_color = phase_color(m.s.phase),
//       .queued      = m.ui.composer.queued.size(),
//       .profile     = {.label = profile_label(m.d.profile),
//                       .color = profile_color(m.d.profile)},
//       .expanded    = m.ui.composer.expanded,
//   }}.build();

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../app/app.hpp"        // request_animation_frame
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include "divider.hpp"

namespace maya {

class Composer {
public:
    // Drives border color, prompt boldness, placeholder text, and the
    // height-pin behaviour that keeps the box from bobbing during
    // streaming / tool execution.
    enum class State : std::uint8_t {
        Idle,                  // nothing happening — may or may not have text
        AwaitingPermission,    // user must respond to a permission prompt above
        Streaming,             // model is generating
        ExecutingTool,         // a tool is running
    };

    struct ProfileChip {
        std::string label;     // raw — widget small-caps's it
        Color       color = Color::magenta();
    };

    struct Config {
        std::string text;
        int         cursor = 0;

        State state         = State::Idle;
        // The color used for the border / prompt while the agent is
        // active (Streaming / ExecutingTool). Caller picks based on the
        // current phase so the composer matches the status bar.
        Color active_color  = Color::cyan();

        // Brand palette
        Color text_color      = Color::bright_white();
        Color accent_color    = Color::magenta();   // "primed" border, idle + text
        Color warn_color      = Color::yellow();    // awaiting-permission border
        Color highlight_color = Color::cyan();      // queue-depth chip

        // Right-side ambient indicators
        std::size_t queued = 0;
        ProfileChip profile;

        // Layout
        bool expanded = false;

        // Minimum body-row count. The composer pads its inner column
        // with blank rows up to this floor so transient height
        // changes (empty→one char, last char→empty, 1-line→2-line
        // wrap) don't reshape the box and reflow the diff against
        // every row above. Default 1 = legacy behavior. Set to 2 (or
        // higher) when the caller wants stable composer height to
        // suppress streaming-time flicker. Caps at the natural body
        // height — never SHRINKS a composer that already has more
        // content than the floor.
        int min_body_rows = 1;
    };

    explicit Composer(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const Color muted = Color::bright_black();
        const bool  has_text     = !cfg_.text.empty();
        const bool  is_awaiting  = (cfg_.state == State::AwaitingPermission);
        const bool  is_streaming = (cfg_.state == State::Streaming);
        const bool  is_executing = (cfg_.state == State::ExecutingTool);
        const bool  active       = is_streaming || is_executing;

        // ── State-driven box / prompt color.
        Color box_color =
            is_awaiting ? cfg_.warn_color :
            active      ? cfg_.active_color :
            has_text    ? cfg_.accent_color :
                          muted;

        // ── Cursor injection — blinking block.
        //
        // We can't drive the real terminal hardware cursor from inside a
        // widget, so the cursor is a glyph painted into the text. We
        // ALWAYS emit U+2588 FULL BLOCK ('█') and toggle its visibility
        // by swapping the STYLE between visible (fg=text) and invisible
        // (fg=bg, i.e. transparent against the box bg). Using one stable
        // byte sequence is critical: any phase-dependent byte length
        // would reflow downstream wrap caches (width vs bytes) and
        // cause a one-cell jitter on every blink — the symptom users
        // perceive as composer flicker. Style-only toggles never
        // reflow.
        //
        // Blink is suppressed while the agent is streaming or running
        // a tool: in those states the user can still type (input
        // queues), and a steady cursor reads as "yes, your keystrokes
        // are landing somewhere" rather than competing with the
        // spinner for the eye.
        //
        // RAF is requested ONLY when the blink phase actually toggles
        // a visible cell — i.e., the composer is idle (blink active)
        // AND a cursor will be painted (always true; placeholder path
        // also paints one). Calling RAF unconditionally would pin the
        // app to 2 Hz repaints even when nothing on screen changes.
        // Cursor blink driven by the maya animation framework. A 530 ms
        // square wave: visible for the first half of each period, hidden for
        // the second. anim::loop_phase owns the wall-clock read AND the
        // frame request, so the composer no longer hand-rolls steady_clock /
        // bucket math / request_animation_frame — it just asks "where in the
        // blink cycle am I?" The phase is only consulted (and frames only
        // requested) while idle; a streaming/executing composer holds a
        // steady cursor.
        constexpr double kBlinkPeriodMs = 530.0;
        const bool blink_off =
            !active && anim::loop_phase(kBlinkPeriodMs) >= 0.5;

        std::string with_cursor = cfg_.text;
        int cur = std::min<int>(cfg_.cursor, static_cast<int>(cfg_.text.size()));
        // Always insert the FULL BLOCK byte sequence; visibility is a
        // style decision below. Same bytes every frame ⇒ same wrap.
        with_cursor.insert(static_cast<std::size_t>(cur),
                           "\xe2\x96\x88");

        // ── Prompt chip + body rows.
        Style prompt_style = (active || has_text || is_awaiting)
            ? Style{}.with_fg(box_color).with_bold()
            : Style{}.with_fg(box_color).with_dim();
        auto prompt_chip  = text("\xe2\x9d\xaf ", prompt_style);              // ❯
        auto continuation = text("\xe2\x94\x8a ", fg_dim_(muted));            // ┊
        auto blank_pre    = text("  ");

        std::vector<Element> body_rows;
        if (!has_text) {
            // When queued items exist and the composer is empty, lead
            // with the recall affordance — host's ↑ keybinding pulls
            // them back into the buffer for editing. Mirrors Claude
            // Code's "Press up to edit queued messages" hint (binary
            // offset 84591379). Suffix the contextual phase verb so
            // the user keeps the "type to add another" affordance
            // visible too.
            std::string placeholder;
            if (cfg_.queued > 0) {
                placeholder = cfg_.queued == 1
                    ? std::string{"press \xe2\x86\x91 to edit queued"}            // ↑
                    : std::string{"\xe2\x86\x91 drain queue • \xe2\x8c\xa5\xe2\x86\x91/\xe2\x8c\xa5\xe2\x86\x93 cycle items"};  // ↑ / ⌥↑ ⌥↓
                if (is_awaiting)       placeholder += " \xe2\x80\x94 awaiting permission above\xe2\x80\xa6";
                else if (is_executing) placeholder += " \xe2\x80\x94 type to queue another\xe2\x80\xa6";
                else if (is_streaming) placeholder += " \xe2\x80\x94 type to queue another\xe2\x80\xa6";
                else                   placeholder += " \xe2\x80\x94 or type a new message\xe2\x80\xa6";
            } else {
                placeholder =
                    is_awaiting  ? "awaiting permission \xe2\x80\x94 respond above\xe2\x80\xa6" :
                    is_executing ? "running tool \xe2\x80\x94 type to queue\xe2\x80\xa6"        :
                    is_streaming ? "streaming \xe2\x80\x94 type to queue\xe2\x80\xa6"           :
                                   "type a message\xe2\x80\xa6";
            }
            body_rows.push_back(h(
                prompt_chip,
                // Stable bytes: always emit █, toggle visibility via
                // style. Same rationale as the with-text path —
                // changing byte length on blink reflows wrap caches.
                text("\xe2\x96\x88",
                     blink_off
                         ? Style{}.with_fg(muted).with_dim()
                         : Style{}.with_fg(muted)),
                text(placeholder, Style{}.with_fg(muted).with_italic())
            ).build());
        } else {
            // Style-driven cursor blink: split each line on the cursor
            // placeholder (which we just inserted as the FULL BLOCK
            // byte sequence) and emit the cursor as its own TextElement
            // whose foreground toggles between visible and the box
            // background. Byte layout is stable across the blink phase
            // — only the SGR color attribute changes — so word-wrap
            // never reflows.
            constexpr std::string_view kBlock = "\xe2\x96\x88";
            const Style text_style    = Style{}.with_fg(cfg_.text_color);
            const Style cursor_visible = Style{}.with_fg(cfg_.text_color);
            // "Invisible" cursor: dim foreground in the box color so
            // the cell stays the same width but reads as empty. Using
            // box bg directly would require knowing the parent's
            // resolved bg, which the widget doesn't have access to;
            // dimming to the box border color hides the glyph against
            // the box chrome reliably across themes.
            const Style cursor_hidden = Style{}.with_fg(box_color).with_dim();
            auto lines = split_lines(with_cursor);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                Element prefix = (i == 0) ? prompt_chip
                                          : (lines.size() > 1 ? continuation : blank_pre);
                std::string_view line = lines[i];
                // Find the cursor placeholder in this line (at most
                // one per line, since we inserted exactly one). If
                // present, emit the line as ONE TextElement with three
                // StyledRuns (pre / cursor / post) instead of three
                // sibling TextElements in a row. Sibling text elements
                // are independent flex items — when `pre` word-wraps
                // internally and overflows to a second visual row, the
                // cursor element stays glued to the end of row 1 at the
                // wrap boundary, so the visible cursor doesn't follow
                // the user's text onto row 2. A single TextElement with
                // runs lets the wrap engine position the cursor cell at
                // whatever visual column its byte offset lands on.
                auto cur_pos = line.find(kBlock);
                if (cur_pos == std::string_view::npos) {
                    body_rows.push_back(h(
                        prefix,
                        text(std::string{line}, text_style)
                    ).build());
                } else {
                    TextElement te;
                    te.content = std::string{line};
                    te.style   = text_style;
                    te.wrap    = TextWrap::Wrap;
                    te.runs.push_back(StyledRun{
                        .byte_offset = 0,
                        .byte_length = cur_pos,
                        .style       = text_style});
                    te.runs.push_back(StyledRun{
                        .byte_offset = cur_pos,
                        .byte_length = kBlock.size(),
                        .style       = blink_off ? cursor_hidden : cursor_visible});
                    te.runs.push_back(StyledRun{
                        .byte_offset = cur_pos + kBlock.size(),
                        .byte_length = line.size() - cur_pos - kBlock.size(),
                        .style       = text_style});
                    body_rows.push_back(h(prefix, Element{std::move(te)}).build());
                }
            }
        }

        // ── Sizing: let the inner column size itself from its rendered
        // rows so soft-wrapped long lines actually push the composer's
        // top edge up. Earlier we pinned `height(rows)` from
        // `body_rows.size()` for jitter-stability, but that only counted
        // explicit '\n' breaks — a single long word-wrapped line clipped
        // to 1 row and overflowed visibly. The status bar pins the bottom
        // of the screen, so growing upward is the correct direction and
        // doesn't bob the user's eye.
        // Pad body to the configured floor with blank rows. Stable
        // composer height across short transient text changes
        // (empty placeholder → first char → empty again) means the
        // diff doesn't see a height delta on every keystroke, which
        // would otherwise push every row above by one canvas-Y and
        // force a full repaint via the per-row diff.
        while (static_cast<int>(body_rows.size()) < cfg_.min_body_rows) {
            body_rows.push_back(text(""));
        }
        auto inner = (v(std::move(body_rows)) | padding(0, 1)).build();

        // ── Hint row: width-adaptive left, ambient right.
        auto kbd = [tc = cfg_.text_color](const char* k) {
            return text(k, Style{}.with_fg(tc).with_bold());
        };
        auto lbl = [muted](const char* l) { return text(l, fg_dim_(muted)); };
        auto dot = [muted]() { return text("  \xc2\xb7  ", fg_dim_(muted)); };

        // ── Width-adaptive hint clusters.
        //
        // Both sides progressively shed segments as available width
        // shrinks. The profile chip on the right is the only must-keep
        // anchor — everything else (newline / expand on the left;
        // queued / words / tokens on the right) sheds in priority
        // order once the combined natural width can't fit.
        //
        // Per-segment widths are measured in display columns by
        // tallying the printed glyphs (UTF-8 codepoints counted as
        // 1 column each — the kbd glyphs we use are all narrow).
        // The result is an upper bound the layout pass never
        // exceeds, so the row never wraps or clips the chip.
        //
        // Hysteresis on absolute thresholds isn't enough here: a
        // shrink during streaming (sparkline growing) would cause
        // the right cluster to overflow the left when avail is
        // narrow. Computing required-width vs avail every frame
        // keeps the row coherent across resizes, profile swaps,
        // and ticking word counters.
        auto hint_left_builder = [kbd, lbl, dot](int avail_width) {
            std::vector<Element> out;
            out.push_back(kbd("\xe2\x86\xb5"));           // ↵
            out.push_back(lbl(" send"));
            if (avail_width >= 64) {
                out.push_back(dot());
                out.push_back(kbd("\xe2\x87\xa7\xe2\x86\xb5 / \xe2\x8c\xa5\xe2\x86\xb5"));
                out.push_back(lbl(" newline"));
            }
            if (avail_width >= 94) {
                out.push_back(dot());
                out.push_back(kbd("^E"));
                out.push_back(lbl(" expand"));
            }
            return out;
        };

        // Right-cluster ingredients passed into the lambda by value so
        // each frame's relayout can rebuild the cluster from a snapshot
        // of the current counts. Width-driven sheds happen INSIDE the
        // lambda where `w` is known.
        struct RightInputs {
            bool has_text;
            int queued;
            int words;
            int toks;
            Color highlight_color;
            Color muted_color;
            Color profile_color;
            std::string profile_label;
        };
        RightInputs ri{
            has_text,
            static_cast<int>(cfg_.queued),
            has_text ? word_count(cfg_.text) : 0,
            has_text ? approx_tokens(cfg_.text) : 0,
            cfg_.highlight_color,
            muted,
            cfg_.profile.color,
            std::string{cfg_.profile.label},
        };

        // Approximate column width of a left cluster for a given
        // avail_width (used to compute remaining space for the right
        // cluster). Keep in sync with hint_left_builder: send=6,
        // dot=5, newline glyphs+label=12, expand glyphs+label=9.
        auto left_cols = [](int avail) {
            int w = 1 /*↵*/ + 5 /* send*/;
            if (avail >= 64) w += 5 /*dot*/ + 7 /*⇧↵ / ⌥↵ (7 narrow cells)*/ + 8 /* newline*/;
            if (avail >= 94) w += 5 /*dot*/ + 2 /*^E*/ + 7 /* expand*/;
            return w;
        };

        Element hint_element = component(
            [hint_left_builder, left_cols, ri, kbd, lbl, dot](int w, int /*h*/) -> Element {
                using namespace dsl;
                auto left = hint_left_builder(w);

                // 2-col indent + trailing space + padding(0,1) on both
                // sides eat 6 cols of chrome — subtract before deciding
                // what the right cluster can afford.
                constexpr int kChromeCols = 6;
                const int budget = std::max(0, w - kChromeCols - left_cols(w));

                // Profile chip widths: "▎ " (2) + small-caps label.
                // small_caps_ inserts a space between each char, so a
                // 5-char label renders as 9 cols ("W R I T E").
                const int chip_cols = 2 + static_cast<int>(
                    ri.profile_label.empty() ? 0
                    : ri.profile_label.size() * 2 - 1);

                // queued segment width: "❚ " (2) + 2-wide int + " queued" (7) + dot (5) = 16
                constexpr int kQueuedCols = 16;
                // words segment: 5-wide int + " words" (6) = 11
                constexpr int kWordsCols = 11;
                // separator dot between words and tok = 5
                // tok segment: "~" (1) + 5-wide int + " tok" (4) = 10, + dot (5) = 15
                constexpr int kTokCols = 5 + 10;
                constexpr int kCountersCols = kWordsCols + kTokCols + 5 /*trailing dot*/;

                std::vector<Element> hint_right;

                // Always include the chip — it's the profile-identity
                // anchor. If even the chip can't fit, the row falls
                // back to chip-only with no left cluster (`left` shrinks
                // to just "↵ send" at narrow widths via avail_width
                // thresholds; if even that overflows, the spacer eats
                // the slack and maya clips the row, but the chip stays
                // visible because it's on the right edge).
                const bool show_queued   = ri.queued > 0
                    && budget >= chip_cols + kQueuedCols;
                const bool show_counters = ri.has_text
                    && budget >= chip_cols
                        + (show_queued ? kQueuedCols : 0)
                        + kCountersCols;

                if (show_queued) {
                    hint_right.push_back(text("\xe2\x9d\x9a ",
                        Style{}.with_fg(ri.highlight_color)));
                    hint_right.push_back(text(
                        tabular_int_(ri.queued, 2) + " queued",
                        Style{}.with_fg(ri.highlight_color).with_bold()));
                    hint_right.push_back(dot());
                }
                if (show_counters) {
                    hint_right.push_back(text(
                        tabular_int_(ri.words, 5) + " words", fg_dim_(ri.muted_color)));
                    hint_right.push_back(text("  \xc2\xb7  ", fg_dim_(ri.muted_color)));
                    hint_right.push_back(text(
                        "~" + tabular_int_(ri.toks, 5) + " tok", fg_dim_(ri.muted_color)));
                    hint_right.push_back(dot());
                }
                hint_right.push_back(text("\xe2\x96\x8e",
                    Style{}.with_fg(ri.profile_color)));
                hint_right.push_back(text(" "));
                hint_right.push_back(text(
                    small_caps_(ri.profile_label),
                    Style{}.with_fg(ri.profile_color).with_bold()));

                // 2-col indent so the hint row lines up with body text:
                // `inner` has padding(0,1) (→1 col left), and body row 0
                // starts with the "❯ " prompt (→2 cols). Match that exactly
                // by adding padding(0,1) here and prepending 2 spaces so
                // ↵ sits directly under the first body character.
                return (h(
                    text("  "),
                    h(left),
                    spacer(),
                    h(hint_right),
                    text(" ")
                ) | padding(0, 1)).build();
            });

        // ── Box composition with optional bottom-right line-count caption.
        int line_count = static_cast<int>(split_lines(cfg_.text).size());

        // ── Divider between body and hint row — hairline rule in the
        // box color so the input area visually splits from the
        // chrome (key hints + ambient counters + profile chip).
        // Left padding = 3 to clear `inner`'s padding(0,1) plus the
        // 2-col "❯ " prompt, so the rule's left end sits exactly under
        // the first body character. Right padding = 1 matches `inner`.
        auto rule = (Divider{DividerConfig{
            .line       = BorderStyle::Single,
            .line_style = Style{}.with_fg(box_color).with_dim(),
        }}.build() | padding(0, 1, 0, 3)).build();

        auto box = v(inner, std::move(rule), std::move(hint_element))
                   | border(BorderStyle::Round)
                   | bcolor(box_color);

        if (line_count > 1) {
            box = std::move(box) | btext(
                " " + std::to_string(line_count) + " lines ",
                BorderTextPos::Bottom, BorderTextAlign::End);
        }
        // No `| grow(1.0f)` here: the composer is a natural-height
        // sibling of the Thread inside AppLayout's column. The Thread
        // is the ONLY element that should grow vertically; giving the
        // composer a non-zero grow factor makes it compete with the
        // Thread for slack space, and during streaming — when the
        // Thread's natural height oscillates with every delta — the
        // composer's allocated rows oscillate too (visible as flicker)
        // or get squeezed to zero (composer disappears entirely until
        // a terminal resize forces a relayout). Width-fill stays
        // intact because AppLayout's column applies the parent's
        // default cross-axis Stretch.
        return box.build();
    }

private:
    Config cfg_;

    // ── Helpers ───────────────────────────────────────────────────────────

    static std::vector<std::string_view> split_lines(std::string_view s) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                out.emplace_back(s.data() + start, i - start);
                start = i + 1;
            }
        }
        out.emplace_back(s.data() + start, s.size() - start);
        return out;
    }

    static int word_count(std::string_view s) {
        int n = 0;
        bool in_word = false;
        for (char c : s) {
            bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (!ws && !in_word) { ++n; in_word = true; }
            else if (ws)         { in_word = false; }
        }
        return n;
    }

    // ~4 bytes/token Claude heuristic — close enough for live counter UI.
    static int approx_tokens(std::string_view s) {
        return static_cast<int>((s.size() + 3) / 4);
    }

    // Right-aligned fixed-width int — keeps surrounding chips pinned as
    // counters tick.
    static std::string tabular_int_(int n, int width) {
        std::string s = std::to_string(n);
        if (static_cast<int>(s.size()) >= width) return s;
        return std::string(static_cast<std::size_t>(width - static_cast<int>(s.size())), ' ')
             + s;
    }

    // Letter-spaced uppercase ("D O N E") for short labels.
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

    // bright_black is already the "subdued secondary" tone — stacking
    // SGR `dim` on top can collapse below readability on some themes,
    // so suppress dim when the color is already bright_black.
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
