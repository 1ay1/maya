#pragma once
// maya::anim::text_reveal — the streaming TYPEWRITER as a framework primitive
// ============================================================================
//
// "Text materialising in real time" — the scramble→resolve tip, the hot→cool
// gradient trail, the ghosted not-yet-revealed body, the bright sweep cursor
// at the reveal front, and the pulsing end-caret — is one of the most
// recognisable maya effects. It used to live as ~300 lines welded into the
// markdown streaming widget, unreachable by any other widget.
//
// This header lifts the VISUAL ALGORITHM out into a reusable decorator that
// operates on the trailing edge of ANY text leaf. A widget that streams text
// (a log tail, a chat bubble, a code preview, a custom renderer) gets the
// full effect by:
//
//   1. building its Element tree as normal,
//   2. finding the rightmost TextElement leaf (the live tail),
//   3. calling anim::decorate_text_reveal(leaf, params).
//
// The decorator is PURELY VISUAL and HEIGHT-STABLE: it never adds or removes
// codepoints (the leaf's wrapped height is identical before and after), so it
// cannot reflow the layout or push committed rows into scrollback. It only
// rewrites the StyledRuns over the trailing window. That invariant is what
// makes it safe to drop into any widget without re-deriving the scrollback
// correctness work.
//
// Time + cursor state are INPUTS — the decorator holds no clock and no
// cursor. The caller supplies `ms_total`, the age of the trailing edge, and
// how many codepoints are "revealed" vs the total. Pair it with
// anim::RateCursor (the typewriter integrator in animation.hpp) and a
// per-frame clock read for the full self-driving experience, or feed it a
// fixed cursor for a static one-shot.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/animation.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya::anim {

// ============================================================================
// TextRevealParams — what the decorator needs to know each frame
// ============================================================================
struct TextRevealParams {
    // Monotonic wall-clock in ms (e.g. anim::Clock::now_ms()). Drives the
    // scramble churn, gradient phase, sweep + caret pulses.
    std::int64_t ms_total = 0;

    // Age in ms of the NEWEST codepoint at the trailing edge — i.e. how long
    // since the tail last grew. 0 = just arrived (full heat), large = settled
    // (cool). Each earlier codepoint is treated as `char_step_ms` older.
    std::int64_t edge_age_ms = 0;

    // Reveal cursor, in codepoints. `revealed_cp` codepoints of the tail
    // leaf's content are considered "typed"; the rest (up to total_cp) are
    // ghosted (rendered invisible but space-occupying) so the text appears to
    // materialise left-to-right. Set revealed_cp == total_cp for no ghosting
    // (decoration only). total_cp == 0 ⇒ derive from the leaf content.
    std::size_t revealed_cp = 0;
    std::size_t total_cp    = 0;

    // When the host clips the visible bytes mid-line (so only completed lines
    // render full-width, scrollback-safe), pass the count of within-line
    // codepoints PAST the cursor here; they get ghosted. When 0, the
    // decorator falls back to (total_cp - revealed_cp).
    std::size_t clipped_unrevealed_cp = 0;
    bool        clip_active           = false;

    // ── tunables (sensible defaults match the original markdown reveal) ──
    std::size_t  trail_len    = 36;   // gradient band length (cp)
    std::size_t  scramble_len = 6;    // churn window at the very tip (cp)
    std::int64_t scramble_ms  = 220;  // a cp scrambles for this long after arrival
    std::int64_t char_step_ms = 26;   // per-cp age increment leftward
    std::size_t  ghost_extra  = 96;   // ghost band beyond the gradient

    // Master toggles so a caller can take just the parts it wants.
    bool enable_scramble = true;
    bool enable_gradient = true;
    bool enable_ghost    = true;   // left-to-right materialise
    bool enable_sweep    = true;   // bright cursor at the reveal front
    bool enable_caret    = true;   // pulsing end-caret when caught up

    // How the not-yet-typed (ghost) cp render. The leaf must stay
    // HEIGHT/WIDTH-stable (the markdown streaming path commits rows to native
    // scrollback), so unrevealed cp can't simply be deleted. Two modes:
    //   • ghost_blank = true  (default): each unrevealed cp is replaced by
    //     display-width-matched SPACES — genuinely INVISIBLE (a space paints
    //     nothing) while preserving the exact column count. This is the true
    //     left-to-right typewriter every caller wants: text materialises out
    //     of empty space instead of fading up from a dim-but-readable body.
    //   • ghost_blank = false: keep the real glyph but style it
    //     default-fg + dim. Only invisible when the leaf sits on the terminal
    //     default background; otherwise the body reads as "already there."
    //     Retained for callers that deliberately want a fade-in look.
    bool ghost_blank = true;

    // Result flag: set true by the decorator when the trailing bytes were
    // rewritten (scramble substituted glyphs of different byte width), so the
    // caller knows to invalidate the leaf's wrap cache. Pure recolor frames
    // leave it false (cheap path — no re-wrap).
};

namespace reveal_detail {

// Eased ping-pong pulse in [0,1] over `period_ms`, routed through smoothstep
// so it decelerates at both ends. (Same helper the original overlay used.)
[[nodiscard]] inline double pulse01(std::int64_t ms_total,
                                    std::int64_t period_ms) noexcept {
    if (period_ms <= 0) return 0.0;
    const double phase =
        static_cast<double>(((ms_total % period_ms) + period_ms) % period_ms) /
        static_cast<double>(period_ms);
    const double tri = phase < 0.5 ? (phase * 2.0) : (2.0 - phase * 2.0);
    return ease::smoothstep(tri);
}

// Step back `n` UTF-8 codepoints from the end of `s`. Returns the byte offset.
[[nodiscard]] inline std::size_t utf8_step_back(std::string_view s,
                                                std::size_t n) noexcept {
    std::size_t i = s.size();
    while (n > 0 && i > 0) {
        --i;
        while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
        --n;
    }
    return i;
}

// Curated "digital noise" glyph set for the scramble tip. STRICTLY width-1
// (single terminal column) — a scramble glyph swap MUST preserve the leaf's
// display width or it reflows the line (and in a streaming context can push
// committed rows into scrollback). Do NOT add wide (CJK / emoji) glyphs here;
// the sparkle (✨, width 2) was removed for exactly this reason.
inline constexpr const char* kScrambleGlyphs[] = {
    "#", "$", "%", "&", "*", "+", "=", "?", "@",
    "!", "~", "^", "<", ">", "|", "/", "\\",
    "\xe2\x96\x91", "\xe2\x96\x92", "\xe2\x96\x93",        // ░▒▓
    "\xe2\x96\xa0", "\xe2\x96\xa1",                        // ■□
    "\xe2\x97\x86", "\xe2\x97\x87", "\xe2\x97\x8f", "\xe2\x97\x8b",  // ◆◇●○
    "\xe2\x9c\xa6",                                        // ✦ (width-1 star)
    "\xce\xb1", "\xce\xb2", "\xce\xb3", "\xce\xb4",        // αβγδ
    "\xce\xbb", "\xcf\x80", "\xcf\x83", "\xcf\x86",        // λπσφ
    "0", "1", "7", "8", "X", "Z",
};
inline constexpr std::size_t kScrambleN =
    sizeof(kScrambleGlyphs) / sizeof(kScrambleGlyphs[0]);

[[nodiscard]] inline std::string_view scramble_pick(std::size_t cp_idx,
                                                    std::int64_t age_ms,
                                                    std::int64_t ms_total) noexcept {
    const std::uint64_t time_bucket = static_cast<std::uint64_t>(ms_total / 45);
    std::uint64_t h = 0x9e3779b97f4a7c15ull;
    h ^= cp_idx + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= time_bucket + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(age_ms) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return std::string_view{kScrambleGlyphs[h % kScrambleN]};
}

// Age-banded hot→cool trail colour via the central anim::lerp(Color) so the
// channel math matches the rest of the UI. Returns nullopt past the trail.
[[nodiscard]] inline std::optional<Style> trail_style(std::int64_t age_ms) {
    if (age_ms >= 700) return std::nullopt;
    Color col;
    bool bold = false, dim = false;
    if (age_ms < 120) {
        const double t = ease::smoothstep(age_ms / 120.0);
        col  = lerp(Color::rgb(255, 90, 200), Color::rgb(120, 230, 255), t);
        bold = true;
    } else if (age_ms < 320) {
        const double t = ease::smoothstep((age_ms - 120) / 200.0);
        col  = lerp(Color::rgb(120, 230, 255), Color::rgb(140, 180, 220), t);
        bold = t < 0.5;
    } else {
        const double t = ease::smoothstep((age_ms - 320) / 380.0);
        col  = lerp(Color::rgb(140, 180, 220), Color::rgb(200, 200, 200), t);
        dim  = true;
    }
    Style s = Style{}.with_fg(col);
    if (bold) s = s.with_bold();
    if (dim)  s = s.with_dim();
    return s;
}

} // namespace reveal_detail

// ============================================================================
// clip_text_to_cursor — the TRUE typewriter: cut content at the reveal front
// ============================================================================
// The ghost band in decorate_text_reveal renders not-yet-typed codepoints in
// Color::default_color()+dim. That is genuinely INVISIBLE only when the leaf
// sits on the terminal default background (the markdown streaming path clips
// its content in build(), so unrevealed text is never in the leaf at all). A
// GENERIC caller handing the decorator a FULL-content leaf instead sees the
// not-yet-typed body rendered dimly-but-readable — the text "is already
// there" and the effect runs on top of it.
//
// For a true left-to-right typewriter on a full-content leaf, call this FIRST
// to physically truncate the content to the first `revealed_cp` codepoints,
// then decorate the (now shorter) leaf with revealed_cp == total_cp (i.e. no
// ghost band — there is nothing past the cursor to ghost). The leaf grows a
// codepoint at a time exactly as a real stream would.
//
// HEIGHT NOTE: unlike decorate_text_reveal this CHANGES the leaf height as
// the cursor advances (by design — that is what "typing" looks like). It is
// therefore for INTERACTIVE / one-shot UI (a generic widget), NOT for the
// scrollback-committed streaming markdown path, which must stay height-stable
// and does its own clipping upstream. Returns the surviving byte length.
inline std::size_t clip_text_to_cursor(TextElement& leaf,
                                       std::size_t revealed_cp) {
    if (leaf.content.empty()) return 0;
    // Walk forward revealed_cp codepoints from the start.
    std::size_t i = 0, seen = 0;
    while (i < leaf.content.size() && seen < revealed_cp) {
        ++i;
        while (i < leaf.content.size() &&
               (static_cast<unsigned char>(leaf.content[i]) & 0xC0) == 0x80)
            ++i;
        ++seen;
    }
    if (i >= leaf.content.size()) return leaf.content.size();  // fully typed
    leaf.content.resize(i);
    // Drop any runs that fell past the cut; clip the straddling one.
    if (!leaf.runs.empty()) {
        std::vector<StyledRun> kept;
        kept.reserve(leaf.runs.size());
        for (const auto& r : leaf.runs) {
            if (r.byte_offset >= i) break;
            const std::size_t end = std::min(r.byte_offset + r.byte_length, i);
            kept.push_back(StyledRun{r.byte_offset, end - r.byte_offset, r.style});
        }
        leaf.runs = std::move(kept);
    }
    leaf.cached_width = -1;
    return i;
}

// ============================================================================
// decorate_text_reveal — apply the typewriter trailing-edge effect in place
// ============================================================================
// Rewrites the StyledRuns over the trailing window of `leaf` to produce the
// scramble / gradient / ghost / sweep effect. HEIGHT-STABLE: the leaf's
// content byte length is preserved except for scramble glyph substitution
// (same display width). Returns true if bytes changed (caller should
// invalidate the wrap cache via `leaf.cached_width = -1`).
//
// This is the EXACT algorithm the markdown streaming widget uses, now
// callable by any widget on any text leaf.
[[nodiscard]] inline bool decorate_text_reveal(TextElement& leaf,
                                               const TextRevealParams& p) {
    if (leaf.content.empty()) return false;

    const std::int64_t ms_total = p.ms_total;
    const std::int64_t edge_age = p.edge_age_ms;

    // Resolve total / revealed codepoint counts.
    std::size_t total_cp = p.total_cp;
    if (total_cp == 0) {
        for (std::size_t i = 0; i < leaf.content.size(); ++i)
            if ((static_cast<unsigned char>(leaf.content[i]) & 0xC0) != 0x80)
                ++total_cp;
    }
    const std::size_t revealed_cp = p.revealed_cp ? p.revealed_cp : total_cp;

    const std::size_t unrevealed_cp =
        p.clip_active ? p.clipped_unrevealed_cp
                      : (total_cp > revealed_cp ? total_cp - revealed_cp : 0u);

    const std::string_view orig = leaf.content;

    auto age_for = [&](std::size_t i_from_tail) -> std::int64_t {
        // Age is measured from the REVEAL FRONT (the cursor), not the content
        // end: the cp at the cursor is "just typed" (age == edge_age), and
        // each cp further left is char_step_ms older. cp to the RIGHT of the
        // cursor (not yet typed) are ghosted, so their age is irrelevant. In
        // i_from_tail terms the cursor front is at i_from_tail == unrevealed_cp
        // (or the content end when the cursor has caught up, unrevealed_cp==0).
        // dist = how many revealed cp left of the front this cp sits.
        const std::size_t front = unrevealed_cp;  // first revealed cp
        const std::int64_t dist =
            static_cast<std::int64_t>(i_from_tail) - static_cast<std::int64_t>(front);
        return edge_age + (dist > 0 ? dist : 0) * p.char_step_ms;
    };

    // Trail window: max(gradient band, unrevealed + ghost extension).
    const std::size_t trail_cp_target =
        std::max(p.trail_len,
                 (p.enable_ghost ? unrevealed_cp + p.ghost_extra : 0u));
    const std::size_t trail_byte_start =
        reveal_detail::utf8_step_back(orig, trail_cp_target);
    const std::string_view trail_slice = orig.substr(trail_byte_start);

    // Codepoint boundaries within the trail slice.
    std::vector<std::size_t> cp_offs;
    cp_offs.reserve(trail_cp_target + 1);
    for (std::size_t i = 0; i < trail_slice.size();) {
        cp_offs.push_back(i);
        ++i;
        while (i < trail_slice.size() &&
               (static_cast<unsigned char>(trail_slice[i]) & 0xC0) == 0x80)
            ++i;
    }
    cp_offs.push_back(trail_slice.size());
    const std::size_t trail_n = cp_offs.empty() ? 0 : cp_offs.size() - 1;
    if (trail_n == 0) return false;

    const std::size_t scramble_n =
        p.enable_scramble ? std::min(trail_n, p.scramble_len) : 0;

    std::string new_trail;
    new_trail.reserve(trail_slice.size() + scramble_n * 3);
    std::vector<StyledRun> new_runs;
    new_runs.reserve((leaf.runs.empty() ? 1 : leaf.runs.size()) + trail_n + 2);

    // Carry forward original runs before the trail window.
    if (trail_byte_start > 0) {
        if (leaf.runs.empty()) {
            new_runs.push_back(StyledRun{0, trail_byte_start, leaf.style});
        } else {
            for (const auto& r : leaf.runs) {
                if (r.byte_offset >= trail_byte_start) break;
                const std::size_t end = r.byte_offset + r.byte_length;
                const std::size_t clipped = std::min(end, trail_byte_start);
                new_runs.push_back(StyledRun{r.byte_offset,
                                             clipped - r.byte_offset, r.style});
                if (end > trail_byte_start) break;
            }
        }
    }

    constexpr std::int64_t kSweepMs = 280;

    for (std::size_t k = 0; k < trail_n; ++k) {
        const std::size_t i_from_tail = trail_n - 1 - k;
        const std::int64_t age = age_for(i_from_tail);

        std::string_view real_cp{trail_slice.data() + cp_offs[k],
                                 cp_offs[k + 1] - cp_offs[k]};
        // Scramble the few cp JUST BEHIND the reveal front — the chars that
        // were most recently "typed". In i_from_tail terms the reveal front
        // sits at i_from_tail == unrevealed_cp (the first REVEALED cp); the
        // scramble window is the scramble_n revealed cp immediately left of
        // it: [unrevealed_cp, unrevealed_cp + scramble_n). When the cursor
        // has caught up (unrevealed_cp == 0) this falls on the trailing edge
        // — the freshest arrivals — matching the original clipped behaviour.
        // Anchoring to the cursor (not content END) is what makes a
        // full-content leaf type left-to-right instead of churning glyphs at
        // the far end past the cursor.
        const bool in_scramble =
            scramble_n > 0 &&
            i_from_tail >= unrevealed_cp &&
            i_from_tail <  unrevealed_cp + scramble_n;
        const bool scrambling  = in_scramble && age < p.scramble_ms;

        std::string scramble_owned;
        std::string blank_owned;
        std::string_view emitted;
        const bool is_ghost = p.enable_ghost && i_from_tail < unrevealed_cp;
        const bool is_sweep_head =
            is_ghost && p.enable_sweep && i_from_tail == unrevealed_cp - 1;
        if (scrambling) {
            scramble_owned = std::string{reveal_detail::scramble_pick(
                trail_byte_start + cp_offs[k], age, ms_total)};
            emitted = scramble_owned;
        } else if (is_ghost && p.ghost_blank && !is_sweep_head) {
            // TRUE invisibility, width-exact: replace the not-yet-typed glyph
            // with as many spaces as its display width (1 for ASCII/most, 2
            // for CJK/wide). A space paints nothing, so the cp is genuinely
            // absent — the text appears to type out of empty space — yet the
            // column count is identical, so committed rows never reflow. The
            // sweep-cursor head keeps its real glyph (it's the bright "now
            // typing" cell) and is excluded here.
            const int w = string_width(real_cp);
            blank_owned.assign(static_cast<std::size_t>(w > 0 ? w : 1), ' ');
            emitted = blank_owned;
        } else {
            emitted = real_cp;
        }

        const std::size_t out_off = trail_byte_start + new_trail.size();
        new_trail.append(emitted.data(), emitted.size());
        const std::size_t out_len = (trail_byte_start + new_trail.size()) - out_off;

        // Style priority: scramble > ghost(+sweep) > gradient > base.
        Style s;
        if (scrambling) {
            const bool flick = ((ms_total / 60 + k) & 1) == 0;
            s = Style{}.with_fg(flick ? Color::rgb(255, 80, 180)
                                      : Color::rgb(255, 160, 60))
                       .with_bold();
        } else if (p.enable_ghost && i_from_tail < unrevealed_cp) {
            // Ghost cell. When ghost_blank is on the glyph is already a space
            // (emitted above), so the style barely matters — but keep it dim
            // default so the rare non-blank ghost (sweep head fallthrough)
            // stays subdued. The sweep head overrides with the bright cursor.
            s = Style{}.with_fg(Color::default_color()).with_dim();
            if (p.enable_sweep && i_from_tail == unrevealed_cp - 1) {
                const double pp = reveal_detail::pulse01(ms_total, kSweepMs);
                s = Style{}
                    .with_fg(lerp(Color::rgb(255, 220, 140),
                                  Color::rgb(180, 255, 220), pp))
                    .with_bg(lerp(Color::rgb(60, 50, 20),
                                  Color::rgb(90, 80, 40), pp))
                    .with_bold();
            }
        } else if (auto ts = p.enable_gradient ? reveal_detail::trail_style(age)
                                               : std::nullopt) {
            s = *ts;
        } else {
            s = leaf.style;
        }
        new_runs.push_back(StyledRun{out_off, out_len, s});
    }

    leaf.content.resize(trail_byte_start);
    leaf.content.append(new_trail);
    leaf.runs = std::move(new_runs);

    // Re-wrap when bytes changed shape. Scramble swaps glyphs (possibly
    // different byte width), and ghost_blank replaces real (possibly
    // multi-byte) glyphs with spaces — both change the content bytes even
    // though DISPLAY width is preserved, so the wrap cache (which can memo on
    // content) must be invalidated for the frames they're active.
    const bool scramble_active =
        edge_age < p.scramble_ms +
                   static_cast<std::int64_t>(scramble_n) * p.char_step_ms;
    const bool blanking_active =
        p.enable_ghost && p.ghost_blank && unrevealed_cp > 0;
    if (scramble_active || blanking_active) {
        leaf.cached_width = -1;
        return true;
    }
    return false;
}

// ============================================================================
// decorate_end_caret — pulsing "awaiting next byte" caret on the last glyph
// ============================================================================
// Recolours the final codepoint of `leaf` with a magenta↔cyan pulse. Height-
// and width-stable (no bytes added when the leaf is non-empty). Call this
// INSTEAD of the sweep cursor when the reveal cursor has caught up to the
// edge (nothing left to type) — the two would otherwise compete for the eye.
inline void decorate_end_caret(TextElement& leaf, std::int64_t ms_total,
                               std::int64_t period_ms = 650) {
    const double pp = reveal_detail::pulse01(ms_total, period_ms);
    const Color fg = lerp(Color::rgb(220, 80, 200), Color::rgb(100, 230, 255), pp);
    const Style caret = Style{}
        .with_fg(Color::rgb(fg.r(), fg.g(), fg.b()))
        .with_bg(Color::rgb(fg.r() / 4, fg.g() / 4, fg.b() / 4))
        .with_bold();

    if (!leaf.content.empty()) {
        const std::size_t off = reveal_detail::utf8_step_back(leaf.content, 1);
        if (off < leaf.content.size()) {
            if (leaf.runs.empty())
                leaf.runs.push_back(StyledRun{0, off, leaf.style});
            leaf.runs.push_back(
                StyledRun{off, leaf.content.size() - off, caret});
            return;
        }
    }
    // Empty leaf: emit a single block caret.
    constexpr std::string_view kBlock = "\xe2\x96\x8a"; // ▊
    leaf.content.append(kBlock);
    leaf.runs.push_back(StyledRun{0, kBlock.size(), caret});
    leaf.cached_width = -1;
}

} // namespace maya::anim
