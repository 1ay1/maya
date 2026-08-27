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

    /// Tell the widget how many reasoning CHARACTERS were produced, so the
    /// settled header can show an accurate token estimate. Hosts whose body
    /// lives outside the widget (build_with_body) MUST call this — otherwise
    /// the estimate falls back to the widget's own StreamingMarkdown, which
    /// is empty on that path ("~0 tokens"). No-op to omit for the self-owned
    /// streaming path.
    void set_char_hint(std::size_t chars) noexcept { char_hint_ = chars; }

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
        // A labeled top rule announces the block ("✦ Thinking ───") and — once
        // settled — a matching bottom rule CLOSES it, so the reasoning is a
        // clearly-enclosed, titled section that visually separates from the
        // answer that follows. The body sits indented beneath the label.
        std::vector<Element> rows;
        rows.reserve(3);
        rows.push_back(build_rule(/*with_label=*/true));

        BoxElement indent;
        indent.layout.direction = FlexDirection::Column;
        indent.layout.padding   = Edges<int>{0, 0, 0, 2};
        indent.children.push_back(std::move(body));
        rows.push_back(Element{std::move(indent)});

        // Closing rule only when settled — while live the block is still
        // growing, so a bottom edge would jitter as text streams in.
        if (!live_)
            rows.push_back(build_rule(/*with_label=*/false));

        BoxElement box;
        box.layout.direction = FlexDirection::Column;
        box.children = std::move(rows);
        return Element{std::move(box)};
    }

    // A full-width rule. With a label it's the block's titled top edge
    // ("✦ Thinking ─────", flush-left so the sigil catches the eye); without,
    // it's the plain bottom edge that closes the block. The rule fills the
    // available width via a ComponentElement.
    [[nodiscard]] Element build_rule(bool with_label) const {
        const Color accent = live_ ? cfg_.accent : dim(cfg_.accent);
        const Color rule_c = dim(accent);
        const Style rule_st  = Style{}.with_fg(rule_c);
        const Style label_st = live_
            ? Style{}.with_fg(cfg_.header_word).with_bold()
            : Style{}.with_fg(cfg_.header_word).with_dim();
        const Style sigil_st = Style{}.with_fg(accent);

        std::string word;
        std::string meta;
        if (with_label) {
            word = live_ ? cfg_.live_word : cfg_.settled_word;
            if (live_) {
                word += " ";
                word += std::string{spinner_frame()};
            } else if (reasoning_chars() > 0) {
                meta = "  \xc2\xb7  " + token_estimate() + " tokens";   // ·
            }
        }

        return Element{ComponentElement{
            .render = [=](int w, int) -> Element {
                if (!with_label) {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x80"; // ─
                    return Element{TextElement{.content = std::move(line),
                                               .style = rule_st}};
                }
                // "✦ Word meta ─────" flush-left.
                std::vector<Element> parts;
                parts.push_back(Element{TextElement{
                    .content = "\xe2\x9c\xa6 ", .style = sigil_st}}); // ✦
                parts.push_back(Element{TextElement{
                    .content = word, .style = label_st}});
                if (!meta.empty())
                    parts.push_back(Element{TextElement{
                        .content = meta, .style = rule_st}});
                // Trailing space, then a fill rule for the remaining width.
                // ✦(2) + word + meta + ─ fill; a leading space before the rule.
                int used = 2 + utf8_cols(word) + utf8_cols(meta);
                int fill = w - used - 1; // 1 for the space before the rule
                if (fill < 0) fill = 0;
                std::string rule = " ";
                for (int i = 0; i < fill; ++i) rule += "\xe2\x94\x80";
                parts.push_back(Element{TextElement{
                    .content = std::move(rule), .style = rule_st}});
                return dsl::h(std::move(parts)).build();
            },
            .layout = {},
        }};
    }

    // Column width of a UTF-8 string, treating every non-continuation byte as
    // one column (the label is ASCII words + a 1-col braille spinner + a ·,
    // all width-1) — enough to size the fill rule.
    static int utf8_cols(std::string_view s) {
        int n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    Config cfg_;
    std::shared_ptr<StreamingMarkdown> md_;
    Spinner<SpinnerStyle::Dots> spinner_{Style{}.with_dim()};
    bool live_ = true;
    std::size_t char_hint_ = 0; // host-supplied reasoning length (0 = use md_)

    // A dimmer variant of a color for the settled rail (recede once done).
    static Color dim(Color c) noexcept { return c.darken(0.45f); }

    // Reasoning length in characters: the host hint if set, else the widget's
    // own streamed source (the self-owned path).
    [[nodiscard]] std::size_t reasoning_chars() const noexcept {
        return char_hint_ ? char_hint_ : md_->source().size();
    }

    // Humanized token estimate: "~4" → "820" → "1.2k" → "18k". Roughly
    // 4 chars/token, matching the rest of the app's rough token accounting.
    [[nodiscard]] std::string token_estimate() const {
        const std::size_t toks = (reasoning_chars() + 3) / 4;
        if (toks < 1000) return std::to_string(toks);
        if (toks < 10000) {
            // one decimal: 1.2k
            const std::size_t whole = toks / 1000;
            const std::size_t tenth = (toks % 1000) / 100;
            return std::to_string(whole) + "." + std::to_string(tenth) + "k";
        }
        return std::to_string((toks + 500) / 1000) + "k";
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
