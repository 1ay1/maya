// reveal_fx.cpp — StreamingMarkdown live-mode animated reveal overlay.
//
// render_live_overlay_() is build()'s finalize step. When the widget isn't
// live (or reveal_fx is off, the default) it returns the settled
// cached_build_ untouched. When reveal_fx is on it layers three effects on
// the trailing edge of the live tail — scramble→resolve, a hot→cool age
// gradient over the last ~24 codepoints, and a pulsing inline block caret —
// driven by request_animation_frame, mutating only a shallow copy.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/terminal/ansi.hpp"   // env_supports_synchronized_output()
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"
#include "maya/app/app.hpp"         // request_animation_frame()

namespace maya {

void StreamingMarkdown::request_finalize(int ramp_ms) noexcept {
    // Nothing to ramp if we're not live: the next build() would short-
    // circuit at the !live_ guard and the ramp would just sit unrunnable,
    // wedging finish() behind is_finalizing() forever. The host calls
    // this every frame while settled, so a no-op here on the trailing
    // frames (post-completion) is the natural idempotency.
    if (!live_) return;
    // Idempotent: a ramp already in flight keeps its deadline. Picking
    // the EARLIER deadline if the host calls us again with a shorter
    // window would be defensible, but in practice the host calls us once
    // per frame at settle — the idempotency that matters is "called
    // every frame while settled" (turn.cpp passes through here every
    // render) not recomputing the deadline forward.
    if (finalize_deadline_ms_ != 0) return;
    if (ramp_ms < 0) ramp_ms = 0;
    const auto now_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    finalize_deadline_ms_ = now_ms + ramp_ms;
}

// Advance reveal_cp_ for this frame. Called from build() top so the
// cursor's effect on the live overlay (scramble/gradient/caret aging) is
// current BEFORE the cache short-circuit. Three things this fixes:
//   1. Finalize-ramp completion (which flips live_ off) is observed
//      before the cache check, so the next frame doesn't serve a stale
//      live-mode build.
//   2. The committed-snap below: if commit_range fired during this
//      frame, snap reveal_cp_ forward to committed_cp so the cursor
//      never lags behind visibly-rendered committed content. Without
//      this, on a burst delta that fires commit_range, age_at_tail_ms
//      would stay 0 for hundreds of ms while the cursor crawls past
//      committed bytes the user already sees — the scramble window
//      would never start aging, so the trailing edge stayed in noise
//      mode long after the bytes settled.
//   3. Scratch values (ms_total, total_cp, age_at_tail_ms) are stamped
//      for render_live_overlay_() to consume without re-walking source_
//      or re-reading the clock.
//
// Returns true when the frame's cached_build_ is stale and a rebuild is
// needed (finalize-ramp completed); caller uses this to bump
// build_dirty_ before the cache check.
bool StreamingMarkdown::advance_reveal_cursor_() const {
    if (!live_ || !reveal_fx_) return false;

    const auto now = std::chrono::steady_clock::now();
    const std::int64_t ms_total = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()).count();
    cursor_advance_ms_total_ = ms_total;

    // Grow-time stamp.
    if (source_.size() > last_seen_size_) {
        last_grow_ms_   = ms_total;
        last_seen_size_ = source_.size();
    } else if (source_.size() < last_seen_size_) {
        last_seen_size_ = source_.size();
        last_grow_ms_   = ms_total;
        reveal_cp_      = 0.0;
        reveal_ms_      = ms_total;
    }

    // Total codepoints in source_. Incremental update: cached value
    // covers source_[0 .. cached_total_cp_at_); count just the new bytes.
    // On a size shrink (clear()/set_content rollback) the cache is
    // invalidated and recounted from 0.
    if (cached_total_cp_at_ > source_.size()) {
        cached_total_cp_ = 0;
        cached_total_cp_at_ = 0;
    }
    for (std::size_t i = cached_total_cp_at_; i < source_.size(); ++i)
        if ((static_cast<unsigned char>(source_[i]) & 0xC0) != 0x80)
            ++cached_total_cp_;
    cached_total_cp_at_ = source_.size();
    const std::size_t total_cp = cached_total_cp_;
    cursor_advance_total_cp_ = total_cp;

    // First-call init: stamp the wall-clock baseline and start the
    // cursor at 0. Must run BEFORE the committed-snap below so the
    // snap survives — otherwise the init clobbers the snapped value
    // back to 0 on the first build() after construction.
    if (reveal_ms_ == 0) { reveal_ms_ = ms_total; reveal_cp_ = 0.0; }

    // Codepoint count of the COMMITTED prefix. The cursor must never
    // lag behind committed_ because committed bytes are already rendered
    // as full styled blocks in the prefix — they're visible no matter
    // what the reveal cursor says. Letting reveal_cp_ stay below
    // committed_cp wastes the typewriter budget catching up to bytes
    // the user already sees, and starves the scramble/gradient age
    // counter on the tail (age_at_tail_ms only starts ticking once the
    // cursor reaches the live edge). On a burst delta that fires
    // commit_range, the scramble window would never start aging — the
    // trailing edge stayed in noise mode long after the bytes settled.
    //
    // Snap forward to committed_cp here. Reveals nothing the user
    // hasn't already seen, frees the typewriter to animate ONLY the
    // live tail (the part where reveal makes visual sense).
    if (cached_committed_cp_at_ > committed_) {
        cached_committed_cp_ = 0;
        cached_committed_cp_at_ = 0;
    }
    for (std::size_t i = cached_committed_cp_at_;
         i < committed_ && i < source_.size(); ++i)
        if ((static_cast<unsigned char>(source_[i]) & 0xC0) != 0x80)
            ++cached_committed_cp_;
    cached_committed_cp_at_ = committed_;
    const std::size_t committed_cp = cached_committed_cp_;
    if (reveal_cp_ < static_cast<double>(committed_cp))
        reveal_cp_ = static_cast<double>(committed_cp);

    {
        double elapsed_s = (ms_total - reveal_ms_) / 1000.0;
        if (elapsed_s < 0.0)   elapsed_s = 0.0;
        if (elapsed_s > 0.120) elapsed_s = 0.120;
        reveal_ms_ = ms_total;
        const double backlog = static_cast<double>(total_cp) - reveal_cp_;
        if (backlog <= 0.0) {
            reveal_cp_ = static_cast<double>(total_cp);
        } else {
            const double kFloorCps  = reveal_floor_cps_;
            const double kDrainSecs = reveal_drain_secs_;
            double cps = backlog / kDrainSecs;
            if (cps < kFloorCps) cps = kFloorCps;
            if (finalize_deadline_ms_ != 0) {
                const std::int64_t remaining_ms =
                    finalize_deadline_ms_ - ms_total;
                if (remaining_ms <= 0) {
                    cps = backlog / std::max(elapsed_s, 0.001);
                } else {
                    const double ramp_cps =
                        backlog / (remaining_ms / 1000.0);
                    if (ramp_cps > cps) cps = ramp_cps;
                }
            }
            reveal_cp_ += cps * elapsed_s;
            if (reveal_cp_ > static_cast<double>(total_cp))
                reveal_cp_ = static_cast<double>(total_cp);
        }
    }

    // Ramp completion — flip live_ off. live_'s Tracked<> bumps
    // build_dirty_ for us, but return dirty=true so the caller doesn't
    // double-check.
    if (finalize_deadline_ms_ != 0
        && reveal_cp_ >= static_cast<double>(total_cp))
    {
        finalize_deadline_ms_ = 0;
        live_ = false;
        request_animation_frame();
        return true;
    }

    cursor_advance_age_tail_ms_ =
        (reveal_cp_ >= static_cast<double>(total_cp))
            ? (ms_total - last_grow_ms_)
            : 0;

    return false;
}

const Element& StreamingMarkdown::render_live_overlay_() const {
        if (!live_) return cached_build_;
        if (!reveal_fx_) return cached_build_;

        // ── Geeky-as-fuck live animation ──
        //
        // Three layered effects on the trailing edge of the live tail:
        //
        //   (1) SCRAMBLE → RESOLVE
        //       Newly-arrived chars start as random ASCII/box glyphs
        //       and "decrypt" into the real char over ~180ms. Drives
        //       a strong "text materialising in real time" feel.
        //
        //   (2) HOT → COOL GRADIENT TRAIL
        //       The last ~24 codepoints follow a per-char color
        //       gradient indexed by age:
        //           freshest  →  hot magenta + bold
        //           young     →  bright cyan
        //           settling  →  ice blue, dim
        //           locked    →  base fg
        //       Each char migrates leftward through the gradient as
        //       new ones arrive on the right.
        //
        //   (3) PULSING BLOCK CARET with TRAILING TRACER
        //       Wide block (▊) cycling magenta→cyan, plus a thin
        //       trailing dim half-block to the right that fades over
        //       300ms — makes the caret feel like it's leaving a
        //       trail as it moves forward.
        //
        // All three driven by request_animation_frame so they
        // continue ticking at ~60 fps. Pure visual layer: cached_build_
        // is not touched; every frame builds cached_live_ fresh from
        // a shallow copy + mutated tail-leaf TextElement.
        //
        // The reveal cursor (reveal_cp_) was already advanced this
        // frame by advance_reveal_cursor_(), called from build() top
        // BEFORE its cache short-circuit — so the cursor and the
        // visual decoration are in sync. We just read the scratch.
        const std::int64_t ms_total    = cursor_advance_ms_total_;
        const std::size_t  total_cp    = cursor_advance_total_cp_;
        const std::size_t  revealed_cp = static_cast<std::size_t>(reveal_cp_);
        const std::int64_t age_at_tail_ms = cursor_advance_age_tail_ms_;

        // Walk a copy of cached_build_ down its rightmost spine to
        // the last leaf TextElement. Descends through ComponentElement
        // by materializing it (calling its render()) and REPLACING it in
        // the parent with the materialized Element — so subsequent
        // mutations land in the copy, not in the cached lambda's capture.
        //
        // `eager_render` is set true when the descent reveals that the
        // tail is a structured block (table / list / blockquote / code).
        // Heuristic: the inline-fallback path produces a flat shape
        // (outer vstack → prefix? → tail TextElement) where the tail-slot
        // child is DIRECTLY a TextElement. Any descent that goes deeper
        // than that on the tail side — through a ComponentElement OR
        // through a nested BoxElement — means the tail is an eager
        // render whose rightmost leaf is just one cell / item / token,
        // NOT the full live byte slice. Source-level cursor-cut on that
        // leaf would erase it while the rest of the structure stays
        // visible (garbled rows / empty last cell), so the caller skips
        // the cut for eager renders and only applies the visual edge
        // effects (scramble, gradient, caret).
        bool eager_render = false;
        auto find_last_text = [&eager_render](Element& root) -> TextElement* {
            // Reset descent state on every call — the lambda is invoked
            // twice per frame (trail + caret) and we want the eager_render
            // signal computed fresh each time, not accumulated.
            eager_render = false;
            int  tail_depth        = 0;
            bool entered_tail_slot = false;
            Element* cur = &root;
            for (int step = 0; step < 64; ++step) {
                if (auto* t = std::get_if<TextElement>(&cur->inner)) {
                    return t->content.empty() ? nullptr : t;
                }
                if (auto* b = std::get_if<BoxElement>(&cur->inner)) {
                    if (b->children.empty()) return nullptr;
                    cur = &b->children.back();
                    if (entered_tail_slot) {
                        // We're already past the tail slot — any deeper
                        // Box is a structural shape (list-items Box,
                        // table-rows Box, blockquote Box).
                        if (++tail_depth > 0) eager_render = true;
                    } else {
                        // Stepping out of the outer vstack INTO its
                        // tail slot. From the next step onward, depth
                        // counts.
                        entered_tail_slot = true;
                    }
                    continue;
                }
                if (auto* c = std::get_if<ComponentElement>(&cur->inner)) {
                    // Descent hit a ComponentElement. We cannot
                    // materialize it here — the render lambda needs the
                    // actual avail_w handed down by the layout engine,
                    // and calling c->render(0, 0) at descent time
                    // produces a width-0 layout: tables collapse to
                    // one-cell columns, code blocks rewrap to one
                    // glyph per line, and that corrupted Element
                    // would REPLACE the live ComponentElement in the
                    // animated tree, becoming what the paint pass
                    // renders. (Bug seen as char-chopped tables
                    // mid-stream.) Eager-rendered tails (table /
                    // list / blockquote / codeblock) don't need a
                    // TextElement leaf to splice the cursor into
                    // anyway — the caller skips source-cut and the
                    // visual edge effects do not apply when the
                    // rightmost leaf is structural rather than a
                    // flat text run. Mark eager_render and bail.
                    (void)c;
                    eager_render = true;
                    return nullptr;
                }
                return nullptr;
            }
            return nullptr;
        };

        // Step back N UTF-8 codepoints from end of `s`.
        auto utf8_step_back = [](std::string_view s,
                                 std::size_t n) -> std::size_t {
            std::size_t i = s.size();
            while (n > 0 && i > 0) {
                --i;
                while (i > 0 &&
                       (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
                    --i;
                }
                --n;
            }
            return i;
        };

        // Tunables.
        //
        // kTrailLen covers the gradient band. kScrambleLen is the
        // churn window at the very tip. kGhostExtra is how far PAST
        // the gradient the ghost (faded-fg) overlay extends when the
        // reveal cursor still has bytes to walk — gives the typewriter
        // its visible "materialising" body without changing height.
        constexpr std::size_t  kTrailLen    = 36;
        constexpr std::size_t  kScrambleLen = 6;
        constexpr std::int64_t kScrambleMs  = 220;
        constexpr std::int64_t kCharStepMs  = 26;
        constexpr std::size_t  kGhostExtra  = 96;

        // Effective age for codepoint at position `i_from_tail`
        // (0 = newest, increasing leftward). Newest char's age is
        // the actual wall-clock since source grew; each earlier char
        // is kCharStepMs older. Clamped so even the oldest in the
        // trail has a non-trivial age — prevents the whole trail
        // from snapping color when bytes arrive in a burst.
        auto age_for = [&](std::size_t i_from_tail) -> std::int64_t {
            return age_at_tail_ms +
                static_cast<std::int64_t>(i_from_tail) * kCharStepMs;
        };

        // Trail color: lerp through a 4-stop palette by age.
        //   age 0       → hot magenta (255, 90, 200) + bold
        //   age 120ms   → bright cyan (120, 230, 255) + bold
        //   age 320ms   → ice blue    (140, 180, 220) dim
        //   age 700ms+  → base fg (no override)
        // Returns nullopt for ages past the trail — caller leaves
        // those chars' original style alone.
        auto trail_style = [&](std::int64_t age_ms)
            -> std::optional<Style>
        {
            if (age_ms >= 700) return std::nullopt;
            // Lerp helpers.
            auto lerp = [](double a, double b, double t) {
                return a + (b - a) * t;
            };
            double r, g, b;
            bool bold = false;
            bool dim  = false;
            if (age_ms < 120) {
                const double t = age_ms / 120.0;
                r = lerp(255, 120, t);
                g = lerp( 90, 230, t);
                b = lerp(200, 255, t);
                bold = true;
            } else if (age_ms < 320) {
                const double t = (age_ms - 120) / 200.0;
                r = lerp(120, 140, t);
                g = lerp(230, 180, t);
                b = lerp(255, 220, t);
                bold = t < 0.5;
            } else {
                // 320 → 700: fade from ice blue to invisible-on-base
                // by reducing saturation. We approximate by lerping
                // toward a neutral light gray.
                const double t = (age_ms - 320) / 380.0;
                r = lerp(140, 200, t);
                g = lerp(180, 200, t);
                b = lerp(220, 200, t);
                dim = true;
            }
            Style s = Style{}.with_fg(Color::rgb(
                static_cast<std::uint8_t>(r),
                static_cast<std::uint8_t>(g),
                static_cast<std::uint8_t>(b)));
            if (bold) s = s.with_bold();
            if (dim)  s = s.with_dim();
            return s;
        };

        // Scramble glyphs: a curated set of ASCII / box-drawing /
        // dingbat chars that read as "digital noise". We pick one
        // deterministically from (i_from_tail, ms_total) so a given
        // char's scramble glyph CHANGES across frames — the
        // characteristic Matrix "churning" effect.
        static constexpr const char* kScrambleGlyphs[] = {
            "#", "$", "%", "&", "*", "+", "=", "?", "@",
            "!", "~", "^", "<", ">", "|", "/", "\\",
            "\xe2\x96\x91",  // ░
            "\xe2\x96\x92",  // ▒
            "\xe2\x96\x93",  // ▓
            "\xe2\x96\xa0",  // ■
            "\xe2\x96\xa1",  // □
            "\xe2\x97\x86",  // ◆
            "\xe2\x97\x87",  // ◇
            "\xe2\x97\x8f",  // ●
            "\xe2\x97\x8b",  // ○
            "\xe2\x9c\xa6",  // ✦
            "\xe2\x9c\xa8",  // ✨ (sparkle — occasional accent)
            "\xce\xb1", "\xce\xb2", "\xce\xb3", "\xce\xb4",  // αβγδ
            "\xce\xbb", "\xcf\x80", "\xcf\x83", "\xcf\x86",  // λπσφ
            "0", "1", "7", "8", "X", "Z",
        };
        constexpr std::size_t kScrambleN =
            sizeof(kScrambleGlyphs) / sizeof(kScrambleGlyphs[0]);

        // Cheap deterministic hash for picking a scramble glyph.
        auto scramble_pick = [&](std::size_t cp_idx,
                                 std::int64_t age_ms) -> std::string_view {
            // Quantise time so we don't change glyph EVERY frame —
            // we churn at ~45ms intervals so individual frames don't
            // look like static. (Faster = noisier, slower = sleepier.)
            const std::uint64_t time_bucket =
                static_cast<std::uint64_t>(ms_total / 45);
            std::uint64_t h = 0x9e3779b97f4a7c15ull;
            h ^= cp_idx + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= time_bucket + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= static_cast<std::uint64_t>(age_ms) + 0x9e3779b9
                 + (h << 6) + (h >> 2);
            return std::string_view{kScrambleGlyphs[h % kScrambleN]};
        };

        // Shallow copy of cached_build_; we'll splice a freshly-built
        // tail TextElement onto its rightmost leaf.
        //
        // Note: this copies the outer vstack BoxElement and its
        // children vector. Children are:
        //   [prefix_component, tail_text]  (or one of those alone)
        // The prefix is a ComponentElement — its copy is O(1) (one
        // std::function copy + shared_ptr refcount bump). The actual
        // committed-prefix content lives inside the lambda's captured
        // shared_ptr<CommittedPrefix>; copying the ComponentElement
        // does NOT touch the prefix block trees, and the renderer's
        // component_cache (keyed on hash_id) keeps the painted cells
        // warm across frames. So this copy is O(1) regardless of
        // transcript length — long sessions stay cheap.
        //
        // The tail TextElement copy DOES copy its content std::string
        // which is up to one paragraph / code block of bytes. We
        // mitigate below by mutating only the trailing kTrailLen
        // codepoints in place rather than rebuilding the whole string.
        Element animated_body = cached_build_;
        if (TextElement* tail = find_last_text(animated_body)) {
            // No content-cut here. build() renders the full tail; the
            // reveal effect is purely visual (scramble + gradient on the
            // trailing edge, pulsing caret on the last glyph). Cutting
            // bytes would shrink the tail TextElement's rendered height
            // — the host's chrome (composer / status bar) sees that as
            // a height delta and reflows. The trailing-edge animation
            // delivers the "text materialising" feel without changing
            // layout.
            (void)revealed_cp;
            (void)total_cp;

            const std::string_view orig = tail->content;

            // Find the trail window WITHOUT scanning the entire
            // content. Walk backward from the end byte-by-byte until
            // we've passed kTrailLen codepoints (a UTF-8 codepoint
            // start is any byte whose top 2 bits are not 10). Then
            // index codepoints only within [trail_byte_start, end).
            // O(kTrailLen) per frame, independent of tail length.
            // Unrevealed-cp count: the right-edge slice that the
            // typewriter cursor hasn't reached yet. We map this to a
            // byte offset in the tail by stepping back that many cp
            // from the end. These bytes still render (height-stable)
            // but get a ghosted style (dim fg fade toward bg) so the
            // user sees the text "materialise" left-to-right as the
            // cursor walks. Clamped to total_cp so a fresh widget with
            // revealed_cp < committed_cp doesn't underflow (shouldn't
            // happen given the snap above, defensive).
            const std::size_t unrevealed_cp =
                (total_cp > revealed_cp) ? (total_cp - revealed_cp) : 0u;

            // Trail window is whichever is bigger: the fixed gradient
            // band, or the unrevealed slice PLUS a ghost extension —
            // so the gradient melts smoothly into the revealed body
            // instead of cutting at a hard edge.
            const std::size_t trail_cp_target = std::max(
                kTrailLen, unrevealed_cp + kGhostExtra);

            const std::size_t trail_byte_start =
                utf8_step_back(orig, trail_cp_target);
            const std::string_view trail_slice =
                orig.substr(trail_byte_start);

            // Codepoint boundaries WITHIN the trail slice only.
            std::vector<std::size_t> trail_cp_offs;
            trail_cp_offs.reserve(kTrailLen + 1);
            for (std::size_t i = 0; i < trail_slice.size();) {
                trail_cp_offs.push_back(i);
                ++i;
                while (i < trail_slice.size() &&
                       (static_cast<unsigned char>(trail_slice[i]) & 0xC0) == 0x80)
                    ++i;
            }
            trail_cp_offs.push_back(trail_slice.size());
            const std::size_t trail_n =
                trail_cp_offs.empty() ? 0 : (trail_cp_offs.size() - 1);

            if (trail_n == 0) {
                // Empty trail (shouldn't happen given we checked
                // tail non-empty above, but defensive). Skip
                // mutation; caret row below still paints.
                goto trail_done;
            }

            // Scramble window is the rightmost min(kScrambleLen,
            // trail_n) codepoints.
            const std::size_t scramble_n =
                std::min(trail_n, kScrambleLen);
            const std::size_t scramble_cp_start_local =
                trail_n - scramble_n;

            // Build the new content in-place: prefix bytes are
            // unchanged (we just reference them via the existing
            // string), then we splice trail bytes that may have a
            // scramble glyph substituted for the real cp.
            //
            // Strategy: reserve a tail-sized buffer (small — bounded
            // by trail_slice.size() + ~kScrambleLen * worst-case-utf8
            // width). Append-build the trail. Then assign:
            //   tail->content = prefix_bytes + new_trail_bytes
            //
            // To avoid a full-content copy we use std::string's
            // resize+memcpy idiom: keep the original prefix by
            // truncating to trail_byte_start, then append the new
            // trail.
            std::string new_trail;
            new_trail.reserve(trail_slice.size() + scramble_n * 3);

            // Carry-forward original runs that fall before the trail.
            std::vector<StyledRun> new_runs;
            new_runs.reserve(
                (tail->runs.empty() ? 1 : tail->runs.size())
                + trail_n + 2);

            if (trail_byte_start > 0) {
                if (tail->runs.empty()) {
                    new_runs.push_back(StyledRun{
                        .byte_offset = 0,
                        .byte_length = trail_byte_start,
                        .style       = tail->style,
                    });
                } else {
                    for (const auto& r : tail->runs) {
                        if (r.byte_offset >= trail_byte_start) break;
                        const std::size_t end =
                            r.byte_offset + r.byte_length;
                        const std::size_t clipped =
                            std::min(end, trail_byte_start);
                        new_runs.push_back(StyledRun{
                            .byte_offset = r.byte_offset,
                            .byte_length = clipped - r.byte_offset,
                            .style       = r.style,
                        });
                        if (end > trail_byte_start) break;
                    }
                }
            }

            // Walk the trail codepoint-by-codepoint, emitting either
            // the real char (with a trail color overlay) or a scramble
            // glyph.
            for (std::size_t k = 0; k < trail_n; ++k) {
                const std::size_t i_from_tail = trail_n - 1 - k;
                const std::int64_t age = age_for(i_from_tail);

                std::string_view real_cp{
                    trail_slice.data() + trail_cp_offs[k],
                    trail_cp_offs[k + 1] - trail_cp_offs[k]};

                const bool in_scramble_window =
                    k >= scramble_cp_start_local;
                const bool scrambling =
                    in_scramble_window && age < kScrambleMs;

                std::string_view emitted;
                std::string scramble_owned;
                if (scrambling) {
                    scramble_owned = std::string{
                        scramble_pick(trail_byte_start + trail_cp_offs[k],
                                      age)};
                    emitted = scramble_owned;
                } else {
                    emitted = real_cp;
                }

                const std::size_t out_byte_off =
                    trail_byte_start + new_trail.size();
                new_trail.append(emitted.data(), emitted.size());
                const std::size_t out_byte_len =
                    (trail_byte_start + new_trail.size()) - out_byte_off;

                // Pick the style.
                //
                // Priority (highest first):
                //   scramble      — churning rainbow at the very tip
                //   unrevealed    — ghost (dim, fade-to-bg) for cp the
                //                   typewriter cursor hasn't walked yet
                //   gradient      — hot→cool age band
                //   base          — settled text
                Style s;
                if (scrambling) {
                    const bool flick =
                        ((ms_total / 60 + k) & 1) == 0;
                    s = Style{}
                        .with_fg(flick
                            ? Color::rgb(255,  80, 180)
                            : Color::rgb(255, 160,  60))
                        .with_bold();
                } else if (i_from_tail < unrevealed_cp) {
                    // Unrevealed cp: render INVISIBLE (fg matches the
                    // terminal background) so the cell occupies space
                    // for the wrap engine but draws no ink. This is the
                    // real typewriter — layout sees the full text, the
                    // user sees text appear left-to-right as the cursor
                    // walks. Cells stay occupied so height is stable.
                    //
                    // Color::default_color() targets the terminal's own
                    // bg slot, which renders nothing visible regardless
                    // of light/dark theme. with_dim() further suppresses
                    // any residual contrast on terminals that ignore
                    // default-fg color.
                    s = Style{}.with_fg(Color::default_color()).with_dim();

                    // The very next codepoint about to be revealed (the
                    // "cursor head") gets a bright pulsing highlight so
                    // the user's eye tracks the typewriter sweeping
                    // right. i_from_tail == unrevealed_cp - 1 is the
                    // freshest unrevealed cp — leftmost in the unrevealed
                    // band, immediately right of the last revealed cp.
                    if (i_from_tail == unrevealed_cp - 1) {
                        // Triangle-wave pulse on a fast period for a
                        // crisp "now typing" cue.
                        constexpr std::int64_t kSweepMs = 280;
                        const double phase = static_cast<double>(
                            ms_total % kSweepMs) /
                            static_cast<double>(kSweepMs);
                        const double tri = (phase < 0.5)
                            ? (phase * 2.0)
                            : (2.0 - phase * 2.0);
                        const auto lerp8c = [](double a, double b, double tt) {
                            return static_cast<std::uint8_t>(
                                a + (b - a) * tt);
                        };
                        s = Style{}
                            .with_fg(Color::rgb(
                                lerp8c(255, 180, tri),
                                lerp8c(220, 255, tri),
                                lerp8c(140, 220, tri)))
                            .with_bg(Color::rgb(
                                lerp8c(60, 90, tri),
                                lerp8c(50, 80, tri),
                                lerp8c(20, 40, tri)))
                            .with_bold();
                    }
                } else if (auto ts = trail_style(age)) {
                    s = *ts;
                } else {
                    s = tail->style;
                }
                new_runs.push_back(StyledRun{
                    .byte_offset = out_byte_off,
                    .byte_length = out_byte_len,
                    .style       = s,
                });
            }

            // Truncate to the prefix and append the rebuilt trail.
            // resize(trail_byte_start) does NOT reallocate; the
            // existing capacity is preserved.
            tail->content.resize(trail_byte_start);
            tail->content.append(new_trail);
            tail->runs = std::move(new_runs);
            // Force re-wrap on the copy ONLY when content bytes might
            // have changed (scramble substituted a 1-byte ASCII for a
            // 3-byte box glyph, or vice versa). When only styles moved
            // (gradient/ghost recolor without scramble) trail bytes are
            // byte-identical to the source render's wrap cache — leave
            // cached_width alone, skip the O(content) re-wrap. On a
            // multi-KB code-block tail this avoidance is the difference
            // between smooth and chokes-the-renderer.
            const bool scramble_active =
                age_at_tail_ms < kScrambleMs +
                static_cast<std::int64_t>(scramble_n) * kCharStepMs;
            if (scramble_active) {
                tail->cached_width = -1;
            }
        }
        trail_done:;

        // ── Inline end-caret ──
        //
        // When the typewriter cursor is still walking (unrevealed_cp > 0)
        // we already painted a bright sweep-cursor at the reveal-front
        // above — a second pulse at the tail end would compete with it
        // and confuse the eye. Skip the end-caret in that case. When the
        // cursor HAS caught up, paint the end-caret as the "awaiting next
        // byte" cue.
        const bool cursor_walking =
            cursor_advance_total_cp_ > static_cast<std::size_t>(reveal_cp_);
        if (!cursor_walking) {
        constexpr std::int64_t kCaretPeriodMs = 650;
        const double caret_phase =
            static_cast<double>(ms_total % kCaretPeriodMs)
            / static_cast<double>(kCaretPeriodMs);
        const double tri = (caret_phase < 0.5)
            ? (caret_phase * 2.0)
            : (2.0 - caret_phase * 2.0);
        const std::uint8_t cr = static_cast<std::uint8_t>(
            220.0 + (100.0 - 220.0) * tri);
        const std::uint8_t cg = static_cast<std::uint8_t>(
             80.0 + (230.0 -  80.0) * tri);
        const std::uint8_t cb = static_cast<std::uint8_t>(
            200.0 + (255.0 - 200.0) * tri);
        if (TextElement* tail = find_last_text(animated_body)) {
            const Style caret_style = Style{}
                .with_fg(Color::rgb(cr, cg, cb))
                .with_bg(Color::rgb(cr / 4, cg / 4, cb / 4))
                .with_bold();
            // Byte offset of the LAST codepoint in the tail (UTF-8 step
            // back one). The pulse run covers just that glyph.
            const std::size_t caret_off =
                utf8_step_back(tail->content, 1);
            if (caret_off < tail->content.size()) {
                // Recolor the final glyph in place — no bytes added, so
                // the wrap result is unchanged and no extra row can
                // appear. Materialize the implicit base run for the
                // [0, caret_off) prefix if runs are empty, then push the
                // pulse run over the last codepoint.
                if (tail->runs.empty()) {
                    tail->runs.push_back(StyledRun{
                        .byte_offset = 0,
                        .byte_length = caret_off,
                        .style       = tail->style,
                    });
                }
                tail->runs.push_back(StyledRun{
                    .byte_offset = caret_off,
                    .byte_length = tail->content.size() - caret_off,
                    .style       = caret_style,
                });
                // Same byte length and same display width as before, so
                // the source render's wrap cache is still valid — do NOT
                // invalidate it (re-wrapping every animation frame is
                // wasted O(content) work and was only needed when the
                // appended glyph changed the width).
            } else {
                // Empty tail (no glyph to pulse) — emit a single block
                // caret. The row is empty so a one-column glyph cannot
                // spill to a new wrapped row; the live↔settled seam
                // stays stable.
                constexpr std::string_view kCaretGlyph =
                    "\xe2\x96\x8a"; // ▊ left 3/4 block
                tail->content.append(kCaretGlyph);
                tail->runs.push_back(StyledRun{
                    .byte_offset = 0,
                    .byte_length = kCaretGlyph.size(),
                    .style       = caret_style,
                });
                tail->cached_width = -1;
            }
        }
        } // !cursor_walking

        // Mirror cached_build_'s cross-axis sizing. Stretch is
        // load-bearing here: parent flex layouts use this widget's
        // reported width for space-between math; the BoxElement
        // default of Align::Start collapses the live wrapper to its
        // children's natural width, which then shrinks the message's
        // allotted columns and explodes per-row wrap.
        //
        // No wrapping vstack now — the caret was inlined above so the
        // animated body IS the whole live element.
        cached_live_ = std::move(animated_body);

        // ── Frame-request cadence ──
        //
        // Two distinct clocks meet here, and conflating them is what made
        // the reveal stutter:
        //
        //   • The COLOR/FX phase (scramble glyphs, gradient hue, caret
        //     pulse) only needs to step a few times a second. Bucketing it
        //     avoids tearing the chrome BELOW the live tail on terminals
        //     without DEC 2026 (Apple Terminal, plain xterm, bare tmux),
        //     where each multi-row repaint paints progressively.
        //
        //   • The REVEAL CURSOR (this widget's own typewriter, advanced
        //     above from wall-clock) needs ~60 fps wakes to unfold text
        //     smoothly. The loop only wakes for animation when something
        //     re-armed a frame request; RAF is that something.
        //
        // Fix: while the reveal cursor is still catching up to the live
        // edge OR the edge moved within the last kRevealActiveMs, re-arm at
        // the animation-frame interval (≈16 ms) so the reveal plays out
        // smoothly. Once the cursor reaches the edge and the wire goes
        // quiet, fall back to the color-phase bucket so the residual caret
        // pulse doesn't keep a non-sync terminal tearing at 60 Hz.
        static const std::int64_t kAnimPhaseMs =
            ansi::env_supports_synchronized_output() ? 33 : 100;
        // Grace window after the last observed growth during which we treat
        // the reveal as "in motion" and want fine-grained wakes. Comfortably
        // longer than one host reveal step so a steady 400 c/s feed (a byte
        // every few ms) never lapses out of the active regime between frames.
        constexpr std::int64_t kRevealActiveMs = 250;
        const bool cursor_catching_up =
            reveal_cp_ < static_cast<double>(total_cp);
        const bool reveal_in_motion =
            cursor_catching_up || age_at_tail_ms <= kRevealActiveMs;

        if (reveal_in_motion) {
            // Smooth reveal: one wake per animation frame. Keep the color
            // phase counter current so the first quiescent frame doesn't
            // see a stale bucket and fire a redundant RAF.
            last_anim_phase_ = ms_total / kAnimPhaseMs;
            ::maya::request_animation_frame();
        } else {
            const std::int64_t phase = ms_total / kAnimPhaseMs;
            if (phase != last_anim_phase_) {
                last_anim_phase_ = phase;
                ::maya::request_animation_frame();
            }
        }
        return cached_live_;
}

} // namespace maya
