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
#include <cmath>
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
#include "maya/core/animation.hpp"   // anim::ease::*, anim::lerp(Color)
#include "maya/anim/text_reveal.hpp"  // anim::decorate_text_reveal / end_caret

namespace maya {

// The trailing-edge reveal decoration (scramble / hot→cool gradient / ghost
// materialise / sweep cursor / pulsing end-caret) is now ENTIRELY owned by the
// framework primitive in maya/anim/text_reveal.hpp. This translation unit keeps
// only the StreamingMarkdown-specific glue: advancing the reveal cursor off the
// wire bytes and walking the live tree to the trailing text leaf. All easing /
// pulse / colour math lives in maya::anim so it stays in ONE place and any
// other widget can reuse it. (The former local reveal_detail::pulse01 was dead
// after the Phase-3 lift and has been removed.)

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
#if MAYA_REVEAL_CENTRAL_CURSOR
        // Central integrator path. The RateCursor reproduces the inline
        // arithmetic below bit-for-bit (test_rate_cursor_matches_reveal_fx)
        // — floor-cps ceiling, burst drain, and the finalize-deadline ramp.
        // Keep it in lock-step with reveal_cp_ (the public cursor) every
        // frame: push the committed-snap + pacing in, integrate, read back.
        reveal_rate_cursor_.set_pacing(reveal_floor_cps_, reveal_drain_secs_);
        reveal_rate_cursor_.set_pos(reveal_cp_);          // honour the snap above
        if (finalize_deadline_ms_ != 0) {
            const double remaining_s =
                (finalize_deadline_ms_ - ms_total) / 1000.0;
            reveal_rate_cursor_.set_deadline(remaining_s);
        } else {
            reveal_rate_cursor_.clear_deadline();
        }
        reveal_cp_ = reveal_rate_cursor_.tick(
            static_cast<double>(total_cp), elapsed_s);
#else
        const double backlog = static_cast<double>(total_cp) - reveal_cp_;
        if (backlog <= 0.0) {
            reveal_cp_ = static_cast<double>(total_cp);
        } else {
            // Typewriter pacing. reveal_floor_cps_ is a CEILING: the
            // cursor never walks faster than floor_cps under normal
            // streaming, so each codepoint takes its turn even on a
            // slow byte-by-byte feed (otherwise total_cp grows at the
            // feed rate and a backlog-driven cps catches up instantly,
            // making the reveal invisible). The only exceptions:
            //   • a large backlog (> drain_secs worth at floor_cps)
            //     accelerates to backlog/drain_secs so a burst clears
            //     within its target window;
            //   • the finalize ramp can exceed both, guaranteeing the
            //     cursor reaches the edge by its deadline.
            const double kFloorCps  = reveal_floor_cps_;
            const double kDrainSecs = reveal_drain_secs_;
            const double burst_cps  = backlog / kDrainSecs;
            double cps = (burst_cps > kFloorCps) ? burst_cps : kFloorCps;
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
#endif
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

    // Refresh the visible-byte clip for this frame. With reveal_fx_ on
    // and live_, build()'s tail extraction clamps to this value so
    // render_tail's eager-styled blocks (heading bold, code-fence
    // border, table rules, bullet markers) don't paint ahead of the
    // typewriter cursor. With reveal_fx_ off OR not live, the clip is
    // source_.size() (no-op). Commit gating in append_safe defers ALL
    // commits while live; finish() flushes them in one snap at
    // end-of-stream when the host expects a layout settle anyway.
    if (reveal_fx_ && live_) {
        const std::size_t cursor_cp =
            static_cast<std::size_t>(reveal_cp_);
        const std::size_t new_clip = byte_offset_for_cp(cursor_cp);
        if (new_clip != reveal_byte_clip_) {
            reveal_byte_clip_ = new_clip;
            build_dirty_ = true;  // tail visible window moved — re-render
        }
    } else {
        reveal_byte_clip_ = source_.size();
    }

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

        // Step-back / age / colour / scramble helpers now live in the
        // framework primitive (maya::anim, anim/text_reveal.hpp). The overlay
        // here only walks to the tail leaf (find_last_text above) and hands
        // the tunables below to anim::decorate_text_reveal.

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

            const bool clip_active =
                reveal_fx_
                && reveal_byte_clip_ != static_cast<std::size_t>(-1)
                && reveal_byte_clip_ < source_.size();
            std::size_t unrevealed_cp;
            if (clip_active) {
                // Count cp in [reveal_byte_clip_, end-of-tail-leaf). The
                // tail leaf ends at the line-granular visible_end; the
                // bytes past reveal_byte_clip_ are the unrevealed slice of
                // the current line. Count directly off source_ (cheap:
                // bounded by one line's length).
                const std::size_t clip_b =
                    std::min(reveal_byte_clip_, source_.size());
                std::size_t line_end = clip_b;
                while (line_end < source_.size()
                       && source_[line_end] != '\n')
                    ++line_end;
                std::size_t cnt = 0;
                for (std::size_t i = clip_b; i < line_end; ++i)
                    if ((static_cast<unsigned char>(source_[i]) & 0xC0)
                            != 0x80)
                        ++cnt;
                unrevealed_cp = cnt;
            } else {
                unrevealed_cp = (total_cp > revealed_cp)
                    ? (total_cp - revealed_cp) : 0u;
            }

            // ── Delegate the trailing-edge decoration to the framework ──
            //
            // The scramble / hot→cool gradient / ghost-materialise / sweep
            // cursor algorithm now lives in maya::anim::decorate_text_reveal
            // (anim/text_reveal.hpp) — a reusable primitive any widget can
            // call on any text leaf. The markdown widget keeps only the
            // tree-specific glue: walking to the live tail leaf (above) and
            // computing the clip-aware unrevealed-cp span (above). The
            // visual algorithm itself is shared. The decorator is
            // height-stable (no cp added except equal-width scramble
            // substitutions) so the scrollback-safety invariants are
            // preserved exactly. Tunables below mirror the historical
            // constants (kTrailLen / kScrambleLen / kScrambleMs / kCharStepMs
            // / kGhostExtra) so behaviour is unchanged byte-for-byte.
            anim::TextRevealParams rp;
            rp.ms_total              = ms_total;
            rp.edge_age_ms           = age_at_tail_ms;
            rp.revealed_cp           = revealed_cp;
            rp.total_cp              = total_cp;
            rp.clip_active           = clip_active;
            rp.clipped_unrevealed_cp = unrevealed_cp;
            rp.trail_len             = kTrailLen;
            rp.scramble_len          = kScrambleLen;
            rp.scramble_ms           = kScrambleMs;
            rp.char_step_ms          = kCharStepMs;
            rp.ghost_extra           = kGhostExtra;
            (void)anim::decorate_text_reveal(*tail, rp);
        }

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
            // Pulsing "awaiting next byte" caret on the last glyph —
            // delegated to the framework primitive (anim::decorate_end_caret
            // in anim/text_reveal.hpp). magenta↔cyan over a 650 ms period,
            // recoloured in place (height/width-stable) so no extra row can
            // appear at the live↔settled seam.
            if (TextElement* tail = find_last_text(animated_body)) {
                anim::decorate_end_caret(*tail, ms_total, 650);
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
        // The reveal needs ~60fps wakes ONLY while it actually has something
        // new to show each frame: either the cursor is still walking toward
        // the live edge (cursor_catching_up), or the freshest cp are still
        // inside the scramble/gradient "hot" window and visibly changing
        // colour. Once the cursor reaches the edge AND the tip has cooled, the
        // overlay is BYTE- and STYLE-identical frame to frame — re-arming
        // 60fps then is pure wasted CPU (a full UI repaint + terminal write
        // that paints the same pixels). The old code held 60fps for a flat
        // 250 ms after every byte; during steady streaming that is ALWAYS
        // true, pinning the whole turn at 60fps. Gate on real visible change
        // instead: the calm color-phase bucket (33/100 ms) covers the
        // residual caret pulse once the reveal settles.
        const bool cursor_catching_up =
            reveal_cp_ < static_cast<double>(total_cp);
        // "Hot" = the trailing edge is young enough that scramble or the
        // hot end of the gradient is still animating it. Past this the trail
        // is settled and only the (slow) caret pulse remains.
        constexpr std::int64_t kHotTailMs = 360;
        const bool tail_still_hot = age_at_tail_ms <= kHotTailMs;
        const bool reveal_in_motion = cursor_catching_up || tail_still_hot;

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
