#pragma once
// maya::widget::reasoning — ReasoningStream: a first-class, streaming
// reasoning / chain-of-thought block.
//
// This is the widget every host should use to show a model "thinking". It
// OWNS a StreamingMarkdown internally, so reasoning text streams exactly like
// normal answer text — incremental parse, a rate-smoothed reveal cursor, and
// the renderer's component cache — instead of re-parsing a static blob every
// frame. The chrome around it is tuned for a calm, legible "watch it think"
// experience:
//
//   ┃ ✦ Thinking ⠋                       ← animated header (live)
//   ┃ The user wants a uniform reasoning
//   ┃ display. First I'll look at how the
//   ┃ answer body streams so I can reuse …
//
//   ┃ ✦ Reasoned · ~420 tokens           ← settled header (no spinner)
//   ┃ …full reasoning stays here, never folds…
//
// Design decisions (all deliberate, all UX):
//   • A soft indigo LEFT RAIL (a real box border, not per-line prefixes) so
//     the block reads as one distinct, quiet aside and the markdown body can
//     be a real rendered tree (lists, code, emphasis) rather than raw lines.
//   • The body is DIMMED so the answer below always wins the eye; reasoning
//     is context, not the headline.
//   • LIVE shows an animated ✦ + word + spinner and a reveal caret on the
//     streaming edge. SETTLED keeps the FULL text expanded (never collapses
//     to a one-line summary — reasoning you can't re-read is reasoning you
//     can't trust) and swaps the spinner for a token estimate.
//   • Great streaming defaults are applied INTERNALLY (reveal fx, adaptive
//     pacing) so a host gets smooth streaming for free by just feeding bytes.
//
// Usage:
//   ReasoningStream r;
//   r.set_live(true);
//   r.feed(delta);            // or r.set_content(full) each frame
//   ... on end of reasoning ...
//   r.finish();
//   r.set_live(false);
//   Element e = r.build();    // or `Element e = r;`

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/anim_clock.hpp"
#include "../element/box.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "markdown.hpp"
#include "spinner.hpp"

namespace maya {

class ReasoningStream {
public:
    // The rail / accent hue. A soft indigo reads as "meta / thought" without
    // competing with status colors (blue=info, magenta=agent). Hosts may
    // override to match a theme.
    struct Config {
        Color accent      = Color::rgb(0x8b, 0x7f, 0xd6); // muted indigo rail + sigil
        Color header_word = Color::rgb(0x9b, 0x93, 0xc4); // dim lavender header word
        bool  dim_body    = true;                         // reasoning recedes
        std::string live_word    = "Thinking";
        std::string settled_word = "Reasoned";
    };

    ReasoningStream() : ReasoningStream(Config{}) {}
    explicit ReasoningStream(Config cfg) : cfg_(std::move(cfg)) {
        md_ = std::make_shared<StreamingMarkdown>();
        // Smooth streaming, out of the box. These are the same knobs the
        // answer-body path tunes; a host gets them for free.
        md_->set_reveal_fx(true);
        md_->set_reveal_pacing(/*floor_cps=*/45.0, /*drain_secs=*/0.40);
        md_->set_reveal_adaptive(true, /*floor_min=*/25.0, /*floor_max=*/180.0);
        md_->set_live(true);
    }

    // ── Streaming input ──────────────────────────────────────────────────
    /// Append a reasoning delta (the canonical streaming entry point).
    void feed(std::string_view bytes) { md_->feed(bytes); }
    /// Replace the whole reasoning body (for hosts that resend the full
    /// string each frame — e.g. a growing msg.reasoning field).
    void set_content(std::string_view full) { md_->set_content(full); }
    /// Same as set_content, but offloads a large divergent reparse to a
    /// worker thread (loading history / snapshotting) so the render thread
    /// never stalls. Pure-append growth stays on the fast synchronous path.
    void set_content_async(std::string_view full) { md_->set_content_async(full); }
    /// Commit any pending tail at end-of-stream so the settled height is
    /// stable (call once when the reasoning channel closes).
    void finish() { md_->finish(); }

    // ── State ────────────────────────────────────────────────────────────
    /// Live = still receiving reasoning (animated header + reveal caret).
    /// Settled = done (full text stays; spinner → token estimate).
    void set_live(bool live) noexcept {
        live_ = live;
        md_->set_live(live);
    }
    [[nodiscard]] bool is_live() const noexcept { return live_; }

    /// True while ANYTHING is still animating (reveal glide, live caret,
    /// spinner, in-flight async parse). Hosts drive request_animation_frame
    /// off this so the block stays smooth without a wasteful always-on tick.
    [[nodiscard]] bool is_animating() const noexcept {
        return live_ || md_->is_animating();
    }

    /// Advance the header spinner. Optional — build() also derives a frame
    /// from the shared anim clock so the block breathes even if a host never
    /// calls this (deterministic under a test clock).
    void advance(float dt) { spinner_.advance(dt); }

    /// The reasoning text so far (codepoint-clean).
    [[nodiscard]] const std::string& source() const noexcept {
        return md_->source();
    }
    [[nodiscard]] bool empty() const noexcept { return md_->source().empty(); }

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    // ── Render ───────────────────────────────────────────────────────────
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return assemble(build_body());
    }

    /// Render the chrome (header + rail) around a body Element the HOST
    /// already built. This is the integration path for hosts whose
    /// markdown state lives OUTSIDE the widget (e.g. a cross-frame render
    /// cache): they own a persistent StreamingMarkdown, hand its build()
    /// output here, and still get the widget's polished, uniform chrome.
    /// `set_content`/`feed` on this widget's own StreamingMarkdown are then
    /// unused, but is_live()/set_live() still drive the header state.
    [[nodiscard]] Element build_with_body(Element body) const {
        return assemble(dim_wrap(std::move(body)));
    }

private:
    [[nodiscard]] Element assemble(Element body) const {
        std::vector<Element> rows;
        rows.reserve(2);
        rows.push_back(build_header());
        rows.push_back(std::move(body));

        // The block: a left rail (accent while live, dimmed when settled) so
        // the whole reasoning stream reads as one quiet, distinct aside.
        BoxElement box;
        box.layout.direction = FlexDirection::Column;
        box.layout.padding   = Edges<int>{0, 0, 0, 1}; // 1 col gap after rail
        box.border = BorderConfig{
            .style  = BorderStyle::Bold,                // ┃ heavier = a "rail"
            .sides  = BorderSides{false, false, false, true}, // left only
            .colors = BorderColors{
                .left = live_ ? cfg_.accent : dim(cfg_.accent),
            },
        };
        box.children = std::move(rows);
        return Element{std::move(box)};
    }

    Config cfg_;
    std::shared_ptr<StreamingMarkdown> md_;
    Spinner<SpinnerStyle::Dots> spinner_{Style{}.with_dim()};
    bool live_ = true;

    // A dimmer variant of a color for the settled rail (recede once done).
    static Color dim(Color c) noexcept { return c.darken(0.45f); }

    [[nodiscard]] Element build_header() const {
        std::string content;
        std::vector<StyledRun> runs;
        auto push = [&](std::string_view part, Style st) {
            const std::size_t s = content.size();
            content.append(part);
            runs.push_back(StyledRun{s, content.size() - s, st});
        };

        // ✦ sigil in the accent hue.
        push("\xe2\x9c\xa6 ", Style{}.with_fg(live_ ? cfg_.accent
                                                    : dim(cfg_.accent)));

        if (live_) {
            push(cfg_.live_word, Style{}.with_fg(cfg_.header_word).with_bold());
            // Spinner frame off the shared clock (deterministic in tests) so
            // it breathes even without a host advance() call.
            push(" ", Style{});
            push(spinner_frame(), Style{}.with_fg(cfg_.accent).with_dim());
        } else {
            push(cfg_.settled_word, Style{}.with_fg(cfg_.header_word).with_dim());
            const std::size_t approx = (md_->source().size() + 3) / 4;
            push("  \xc2\xb7  ~" + std::to_string(approx) + " tokens", // · ~N tokens
                 Style{}.with_dim().with_italic());
        }

        return Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd, // one row, never wraps
            .runs    = std::move(runs),
        }};
    }

    [[nodiscard]] Element build_body() const {
        return dim_wrap(md_->build());
    }

    [[nodiscard]] Element dim_wrap(Element body) const {
        if (!cfg_.dim_body) return body;
        // Wrap so the whole reasoning tree recedes under the answer. A
        // zero-inset box carrying a dim style tints its subtree without
        // touching layout.
        BoxElement wrap;
        wrap.layout.direction = FlexDirection::Column;
        wrap.style = Style{}.with_dim();
        wrap.children.push_back(std::move(body));
        return Element{std::move(wrap)};
    }

    // Header spinner glyph derived from the shared animation clock so the
    // block animates in lockstep with the rest of the live chrome.
    [[nodiscard]] std::string_view spinner_frame() const {
        static constexpr std::string_view kFrames[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
            "\xe2\xa0\x87", "\xe2\xa0\x8f",
        };
        constexpr std::size_t n = sizeof(kFrames) / sizeof(kFrames[0]);
        const std::size_t i =
            static_cast<std::size_t>((maya::anim_now_ms() / 90)) % n;
        return kFrames[i];
    }
};

} // namespace maya
