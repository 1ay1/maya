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
        // widget (the inline runtime doesn't expose a placement primitive
        // here), so the cursor is a glyph painted into the text. We use
        // U+2588 FULL BLOCK ('█') for the classic terminal cursor look,
        // and blink it by swapping in a same-width SPACE on the "off"
        // half-cycle. 530 ms full period (~265 ms on / 265 ms off) tracks
        // the de-facto VT-style cadence ergonomically.
        //
        // Blink is suppressed while the agent is streaming or running a
        // tool: in those states the user can still type (input queues),
        // and a steady cursor reads as "yes, your keystrokes are landing
        // somewhere" rather than competing with the spinner for the
        // eye.
        //
        // We call request_animation_frame() so maya's event-driven loop
        // (fps=0) keeps repainting while the composer is on-screen with
        // a blinking cursor; without it the cursor would freeze in
        // whichever phase the last user-driven repaint captured.
        constexpr int kBlinkPeriodMs = 530;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        const bool blink_off = !active
            && ((now_ms / (kBlinkPeriodMs / 2)) % 2 != 0);
        if (!active) request_animation_frame();

        std::string with_cursor = cfg_.text;
        int cur = std::min<int>(cfg_.cursor, static_cast<int>(cfg_.text.size()));
        const char* cursor_glyph = blink_off
            ? " "                     // ASCII space (same column width as █)
            : "\xe2\x96\x88";          // █ FULL BLOCK
        with_cursor.insert(static_cast<std::size_t>(cur), cursor_glyph);

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
                text(blink_off ? " " : "\xe2\x96\x88", fg_dim_(muted)),     // █ dim cursor
                text(placeholder, Style{}.with_fg(muted).with_italic())
            ).build());
        } else {
            auto lines = split_lines(with_cursor);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                Element prefix = (i == 0) ? prompt_chip
                                          : (lines.size() > 1 ? continuation : blank_pre);
                body_rows.push_back(h(
                    prefix,
                    text(std::string{lines[i]}, Style{}.with_fg(cfg_.text_color))
                ).build());
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
        auto inner = (v(std::move(body_rows)) | padding(0, 1)).build();

        // ── Hint row: width-adaptive left, ambient right.
        auto kbd = [tc = cfg_.text_color](const char* k) {
            return text(k, Style{}.with_fg(tc).with_bold());
        };
        auto lbl = [muted](const char* l) { return text(l, fg_dim_(muted)); };
        auto dot = [muted]() { return text("  \xc2\xb7  ", fg_dim_(muted)); };

        auto hint_left_builder = [kbd, lbl, dot](int avail_width) {
            std::vector<Element> out;
            out.push_back(kbd("\xe2\x86\xb5"));           // ↵
            out.push_back(lbl(" send"));
            if (avail_width >= 60) {
                out.push_back(dot());
                out.push_back(kbd("\xe2\x87\xa7\xe2\x86\xb5 / \xe2\x8c\xa5\xe2\x86\xb5"));
                out.push_back(lbl(" newline"));
            }
            if (avail_width >= 90) {
                out.push_back(dot());
                out.push_back(kbd("^E"));
                out.push_back(lbl(" expand"));
            }
            return out;
        };

        std::vector<Element> hint_right;
        if (cfg_.queued > 0) {
            hint_right.push_back(text("\xe2\x9d\x9a ",
                                      Style{}.with_fg(cfg_.highlight_color)));   // ❚
            hint_right.push_back(text(
                tabular_int_(static_cast<int>(cfg_.queued), 2) + " queued",
                Style{}.with_fg(cfg_.highlight_color).with_bold()));
            hint_right.push_back(dot());
        }
        if (has_text) {
            int words = word_count(cfg_.text);
            int toks  = approx_tokens(cfg_.text);
            hint_right.push_back(text(
                tabular_int_(words, 4) + " words", fg_dim_(muted)));
            hint_right.push_back(text("  \xc2\xb7  ", fg_dim_(muted)));
            hint_right.push_back(text(
                "~" + tabular_int_(toks, 4) + " tok", fg_dim_(muted)));
            hint_right.push_back(dot());
        }
        hint_right.push_back(text("\xe2\x96\x8e",
                                  Style{}.with_fg(cfg_.profile.color)));         // ▎
        hint_right.push_back(text(" "));
        hint_right.push_back(text(
            small_caps_(cfg_.profile.label),
            Style{}.with_fg(cfg_.profile.color).with_bold()));

        Element hint_element = component(
            [hint_left_builder, hint_right](int w, int /*h*/) -> Element {
                using namespace dsl;
                auto left = hint_left_builder(w);
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
        return (std::move(box) | grow(1.0f)).build();
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
