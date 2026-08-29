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
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/anim_clock.hpp"
#include "../core/animation.hpp"   // anim::lerp(Color,Color,t)
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
        // The reasoning body text COLOR. maya deliberately suppresses the SGR
        // "dim" attribute (it made text vanish on some themes), so recession is
        // carried by COLOR, not the dim flag — we recolor every run of the
        // rendered markdown to this muted gray so reasoning clearly reads as a
        // quiet aside beneath the answer.
        Color body_fg     = Color::rgb(0x8a, 0x8a, 0x8a); // muted gray
        // "Stream of consciousness" gradient: while LIVE, fade the body
        // vertically from `body_fg` at the top (older thoughts, receded) to
        // `body_fg_bright` at the bottom (the newest lines, glowing) so the
        // eye follows the live edge as the model thinks. Settled reasoning
        // renders flat in `body_fg` (a frozen, uniform aside). Off by
        // default — hosts opt in per block.
        bool  gradient_body = false;
        Color body_fg_bright = Color::rgb(0xc9, 0xc2, 0xf0); // bright lavender (newest)
        // Breathe the newest lines + the rail with the animation clock while
        // live, so the block reads as actively thinking (needs gradient_body).
        bool  pulse = false;
        // While live, show only the last N line-nodes of the reasoning (a
        // scrolling "thought ticker") so a long chain-of-thought stays
        // compact and doesn't shove the composer around. 0 = show everything.
        // Settled always shows the full body.
        int   live_tail_lines = 0;
        // STRUCTURE the reasoning into beats: paragraph-leading decision
        // markers ("Let me…", "First", "Actually", "So," …) and markdown
        // emphasis/headers render as bright `waypoint_fg` waypoints while the
        // connective prose recedes — so the block reads as phases, not a gray
        // river. Applies live AND settled. Off by default.
        bool  structured = false;
        Color waypoint_fg = Color::rgb(0xe4, 0xe0, 0xff); // bright lavender-white beat
        // When the host hands build_with_body() a body that already carries
        // its own per-row colors (the faded_tail), skip the chrome's flat
        // recolor so the fade survives.
        bool  body_prestyled = false;
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
    /// settled header shows an accurate token estimate. Hosts whose body
    /// lives outside the widget (build_with_body) MUST call this — otherwise
    /// the estimate falls back to the widget's own StreamingMarkdown, which
    /// is empty on that path ("~0 tokens").
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
        // A pre-styled body (e.g. the faded fixed-height tail) owns its own
        // per-row colors — don't flatten it with the chrome recolor.
        if (cfg_.body_prestyled) return assemble(std::move(body));
        return assemble(dim_wrap(std::move(body)));
    }

    // Build a FIXED-HEIGHT faded tail of `source`: wrap to the render width,
    // keep the LAST `height` visual rows, and fade each row's fg smoothly
    // from `faded` at the top (oldest — dissolving toward the background) to
    // `full` at the bottom (newest). Always exactly `height` rows tall
    // (blank-padded at the top when short) so the block never jumps or grows.
    // Lazy (wraps at layout width); hand to build_with_body() with
    // Config::body_prestyled = true so the chrome doesn't re-recolor it.
    [[nodiscard]] static Element faded_tail(std::string source, int height,
                                            Color faded, Color full) {
        ComponentElement comp;
        comp.render = [source = std::move(source), height, faded, full]
                      (int w, int) -> Element {
            const int width = w > 6 ? w : 48;
            std::vector<std::string> lines = wrap_text(source, width);
            const int H = height > 0 ? height : 1;
            const int content = std::min<int>(static_cast<int>(lines.size()), H);
            const int pad   = H - content;
            const int start = static_cast<int>(lines.size()) - content;
            std::vector<Element> rows;
            rows.reserve(static_cast<std::size_t>(H));
            for (int i = 0; i < H; ++i) {
                // Visual row i (0 = top). Smoothstep fade: bottom rows full,
                // top rows dissolve toward `faded` — a soft high-res gradient.
                const double t = H <= 1 ? 1.0
                    : static_cast<double>(i) / static_cast<double>(H - 1);
                const double a = t * t * (3.0 - 2.0 * t);
                const Color c = maya::anim::lerp(faded, full, a);
                TextElement te;
                if (i >= pad)
                    te.content = lines[static_cast<std::size_t>(start + i - pad)];
                te.style = Style{}.with_fg(c);
                te.wrap  = TextWrap::NoWrap;   // already wrapped to width
                rows.push_back(Element{std::move(te)});
            }
            BoxElement box;
            box.layout.direction = FlexDirection::Column;
            box.children = std::move(rows);
            return Element{std::move(box)};
        };
        return Element{std::move(comp)};
    }

private:
    // Greedy word-wrap `src` to `width` columns, splitting on newlines
    // (blank lines preserved as empty rows). Long words are hard-split.
    // Byte-oriented — fine for the ASCII-dominant reasoning text; a wide
    // rune counts as its bytes, which at worst wraps a hair early.
    static std::vector<std::string> wrap_text(std::string_view src, int width) {
        if (width < 1) width = 1;
        std::vector<std::string> out;
        auto flush_para = [&](std::string_view para) {
            if (para.empty()) { out.emplace_back(); return; }
            std::string line;
            std::size_t p = 0;
            while (p < para.size()) {
                while (p < para.size() && para[p] == ' ') ++p;
                std::size_t we = p;
                while (we < para.size() && para[we] != ' ') ++we;
                std::string_view word = para.substr(p, we - p);
                p = we;
                if (word.empty()) break;
                auto hard_split = [&](std::string_view& w2) {
                    while (static_cast<int>(w2.size()) > width) {
                        out.emplace_back(w2.substr(0, static_cast<std::size_t>(width)));
                        w2.remove_prefix(static_cast<std::size_t>(width));
                    }
                };
                if (line.empty()) {
                    hard_split(word);
                    line.assign(word);
                } else if (static_cast<int>(line.size() + 1 + word.size()) <= width) {
                    line += ' ';
                    line.append(word);
                } else {
                    out.push_back(std::move(line));
                    line.clear();
                    hard_split(word);
                    line.assign(word);
                }
            }
            if (!line.empty()) out.push_back(std::move(line));
            else if (out.empty() || !out.back().empty()) { /* nothing */ }
        };
        std::size_t i = 0;
        while (true) {
            std::size_t nl = src.find('\n', i);
            if (nl == std::string_view::npos) { flush_para(src.substr(i)); break; }
            flush_para(src.substr(i, nl - i));
            i = nl + 1;
        }
        return out;
    }

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
                .left = live_ ? rail_color() : dim(cfg_.accent),
            },
        };
        box.children = std::move(rows);
        return Element{std::move(box)};
    }

    Config cfg_;
    std::shared_ptr<StreamingMarkdown> md_;
    Spinner<SpinnerStyle::Dots> spinner_{Style{}.with_dim()};
    bool live_ = true;
    std::size_t char_hint_ = 0; // host-supplied reasoning length (0 = use md_)

    // A dimmer variant of a color for the settled rail (recede once done).
    static Color dim(Color c) noexcept { return c.darken(0.45f); }

    // Rail hue while live: breathes between dim and full accent in lockstep
    // with the body pulse (same pulse01 phase) so the whole block feels alive
    // while thinking. Flat accent when pulse is off.
    [[nodiscard]] Color rail_color() const {
        if (!(cfg_.pulse && live_)) return cfg_.accent;
        return maya::anim::lerp(dim(cfg_.accent), cfg_.accent,
                                0.45 + 0.55 * pulse01());
    }

    [[nodiscard]] std::size_t reasoning_chars() const noexcept {
        return char_hint_ ? char_hint_ : md_->source().size();
    }

    // Humanized token estimate: "4" → "820" → "1.2k" → "18k" (~4 chars/token).
    [[nodiscard]] std::string token_estimate() const {
        const std::size_t toks = (reasoning_chars() + 3) / 4;
        if (toks < 1000) return std::to_string(toks);
        if (toks < 10000)
            return std::to_string(toks / 1000) + "." +
                   std::to_string((toks % 1000) / 100) + "k";
        return std::to_string((toks + 500) / 1000) + "k";
    }

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
            // Live thinking METER: the token estimate ticks up as reasoning
            // streams, so the block conveys effort/progress, not just motion.
            // Reflects the FULL source even when the body is tail-windowed.
            if (reasoning_chars() > 0)
                push("  \xc2\xb7  " + token_estimate() + " tok",   // · N tok
                     Style{}.with_fg(cfg_.header_word).with_dim().with_italic());
        } else {
            push(cfg_.settled_word, Style{}.with_fg(cfg_.header_word).with_dim());
            // Suppress the token meta at ~0 so a stray empty block never
            // reads "0 tokens".
            if (reasoning_chars() > 0)
                push("  \xc2\xb7  " + token_estimate() + " tokens", // · N tokens
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
        // Recede the whole reasoning tree by COLOR (the maya dim SGR is
        // suppressed, so a with_dim() box does nothing visible). The body is a
        // lazy StreamingMarkdown ComponentElement, so we can't recolor it
        // statically — wrap it in a ComponentElement that renders the inner
        // tree at layout time and then overrides every text run's fg.
        //
        // WHILE LIVE (and gradient_body), fade vertically from body_fg at the
        // top (older thoughts) to body_fg_bright at the bottom (newest) so the
        // eye rides the live edge — a "stream of consciousness". Settled, or
        // gradient off, it's a flat muted recolor (a frozen, uniform aside).
        const bool grad = cfg_.gradient_body && live_;
        const bool pulse = cfg_.pulse && live_;
        const int  tail  = live_ ? cfg_.live_tail_lines : 0;
        const bool structured = cfg_.structured;
        const Color fg     = cfg_.body_fg;
        const Color bright = cfg_.body_fg_bright;
        const Color waypoint = cfg_.waypoint_fg;
        ComponentElement comp;
        comp.render = [body = std::move(body), fg, bright, waypoint,
                       grad, pulse, tail, structured]
                      (int w, int h) -> Element {
            Element rendered = body; // copy the (cheap) wrapper node
            int total = materialize_and_count(rendered, w, h);
            // Tail window: keep only the last `tail` line-nodes (a scrolling
            // thought ticker) so long reasoning stays compact while live.
            if (tail > 0 && total > tail) {
                int pidx = 0;
                prune_to_tail(rendered, total - tail, pidx);
                total = tail;
            }
            // Gradient endpoint (breathing when pulsing); flat = fg→fg.
            Color to = fg;
            if (grad) {
                to = bright;
                if (pulse) {
                    const double k = 0.62 + 0.38 * pulse01();
                    to = maya::anim::lerp(fg, bright, k);
                }
            }
            int idx = 0;
            apply_gradient(rendered, fg, to, idx, total, w, h,
                           structured, waypoint);
            return rendered;
        };
        return Element{std::move(comp)};
    }

    // Pulse phase in [0,1] from the shared animation clock (~1.4 s period),
    // so the body glow and the rail breathe in lockstep.
    [[nodiscard]] static double pulse01() {
        const double ph = static_cast<double>(maya::anim_now_ms()) / 1400.0;
        return 0.5 + 0.5 * std::sin(ph * 6.2831853);
    }

    // Recursively override the foreground color of every text run in a tree.
    // ComponentElement children are rendered (with the given w/h) so their
    // produced subtree is recolored too — this is what reaches the streaming
    // markdown's lazily-built paragraphs.
    static void recolor_fg(Element& e, Color fg, int w, int h) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                node.style = node.style.with_fg(fg);
                for (auto& r : node.runs) r.style = r.style.with_fg(fg);
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                // Leave a background (e.g. code block) intact; only steer fg.
                for (auto& c : node.children) recolor_fg(c, fg, w, h);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items) recolor_fg(c, fg, w, h);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    recolor_fg(inner, fg, w, h);
                    // Replace the lazy node with the recolored, materialized
                    // subtree so the renderer paints the tinted version.
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
            // ElementListRef: borrowed app-owned data — don't mutate.
        }, e.inner);
    }

    // Materialize every lazy ComponentElement into a concrete subtree AND
    // count the TextElement nodes, in traversal (top-to-bottom) order. The
    // count is the number of gradient steps; materializing here means the
    // second pass (apply_gradient) walks the exact same node set/order.
    static int materialize_and_count(Element& e, int w, int h) {
        int n = 0;
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                n = 1;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                for (auto& c : node.children) n += materialize_and_count(c, w, h);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items) n += materialize_and_count(c, w, h);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    n += materialize_and_count(inner, w, h);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
        }, e.inner);
        return n;
    }

    // Does this paragraph START a reasoning beat? True for lines that open
    // with a decision/phase marker — the connective tissue of chain-of-
    // thought. Case-insensitive on the first token, after trimming leading
    // whitespace and markdown list/quote glyphs.
    static bool is_beat(std::string_view s) {
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '>'
                || s[i] == '-' || s[i] == '*' || s[i] == '#' || s[i] == '.'
                || (s[i] >= '0' && s[i] <= '9') || s[i] == ')'))
            ++i;
        s.remove_prefix(i);
        if (s.empty()) return false;
        char buf[20];
        const std::size_t n = std::min<std::size_t>(s.size(), sizeof(buf) - 1);
        for (std::size_t k = 0; k < n; ++k) {
            char c = s[k];
            buf[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        std::string_view p{buf, n};
        static constexpr std::string_view kMarkers[] = {
            "let me", "let's", "lets ", "first", "second", "third", "next",
            "then,", "then ", "finally", "now,", "now ", "actually", "wait",
            "hmm", "so,", "so ", "okay", "ok,", "ok ", "looking at", "i need",
            "i'll", "i should", "i want", "i can", "the key", "the issue",
            "the problem", "the plan", "the goal", "step ", "note:", "important",
            "however", "but ", "instead", "alternatively", "to summar",
            "in summary", "therefore", "because", "given ", "considering",
            "overall", "hold on", "on second", "checking", "i'm going",
        };
        for (auto m : kMarkers) if (p.starts_with(m)) return true;
        return false;
    }

    // Recolor each TextElement to lerp(from,to, idx/(total-1)) in traversal
    // order — idx 0 (top / oldest) = from, idx total-1 (bottom / newest) = to.
    // When `structured`, paragraphs that OPEN a beat (is_beat) and any bold
    // run (markdown emphasis / headers) get `waypoint` + bold instead, so the
    // reasoning reads as phases. Assumes the tree is materialized.
    static void apply_gradient(Element& e, Color from, Color to,
                               int& idx, int total, int w, int h,
                               bool structured = false,
                               Color waypoint = {}) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                const double t = total <= 1 ? 1.0
                    : static_cast<double>(idx) / static_cast<double>(total - 1);
                const Color base = maya::anim::lerp(from, to, t);
                const bool beat = structured && is_beat(node.content);
                if (beat) {
                    node.style = node.style.with_fg(waypoint).with_bold();
                    for (auto& r : node.runs)
                        r.style = r.style.with_fg(waypoint).with_bold();
                } else {
                    node.style = node.style.with_fg(base);
                    for (auto& r : node.runs)
                        r.style = (structured && r.style.bold)
                            ? r.style.with_fg(waypoint)   // inline emphasis = mini-beat
                            : r.style.with_fg(base);
                }
                ++idx;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                for (auto& c : node.children)
                    apply_gradient(c, from, to, idx, total, w, h, structured, waypoint);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items)
                    apply_gradient(c, from, to, idx, total, w, h, structured, waypoint);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    apply_gradient(inner, from, to, idx, total, w, h, structured, waypoint);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
        }, e.inner);
    }

    // Tail window: keep only line-nodes whose global index >= `cutoff` (the
    // last N), pruning earlier text and any container left empty, so a long
    // live reasoning chain collapses to its most-recent lines. `idx` threads
    // the running text-node counter in the SAME order as
    // materialize_and_count. Returns whether `e` should stay in its parent.
    static bool prune_to_tail(Element& e, int cutoff, int& idx) {
        bool keep = true;
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                keep = (idx >= cutoff);
                ++idx;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                std::vector<Element> kept;
                kept.reserve(node.children.size());
                for (auto& c : node.children)
                    if (prune_to_tail(c, cutoff, idx)) kept.push_back(std::move(c));
                node.children = std::move(kept);
                keep = !node.children.empty();
            } else if constexpr (std::is_same_v<T, ElementList>) {
                std::vector<Element> kept;
                kept.reserve(node.items.size());
                for (auto& c : node.items)
                    if (prune_to_tail(c, cutoff, idx)) kept.push_back(std::move(c));
                node.items = std::move(kept);
                keep = !node.items.empty();
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(0, 0);  // materialized → args ignored
                    keep = prune_to_tail(inner, cutoff, idx);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
        }, e.inner);
        return keep;
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
