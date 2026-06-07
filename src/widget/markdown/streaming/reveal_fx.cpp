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

const Element& StreamingMarkdown::render_live_overlay_() const {
        if (!live_) return cached_build_;

        // Animated streaming-reveal (gradient trail + scramble + pulsing
        // caret) is opt-in. Off by default it returns the settled,
        // fully-styled build with no per-frame color/glyph churn and no
        // animation-frame request — text just appears as it streams,
        // flicker-free on every terminal. The height-monotonicity
        // guarantee lives in render_tail (canonical committed-shape
        // floor), not here. See StreamingMarkdown::reveal_fx_.
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

        const auto now = std::chrono::steady_clock::now();
        const std::int64_t ms_total = std::chrono::duration_cast<
            std::chrono::milliseconds>(now.time_since_epoch()).count();

        // Update grow-time stamp: if source has grown since last
        // build(), record this frame's wall time as the moment the
        // trailing edge moved. last_seen_size_ tracks the size we
        // observed last frame.
        if (source_.size() > last_seen_size_) {
            last_grow_ms_   = ms_total;
            last_seen_size_ = source_.size();
        } else if (source_.size() < last_seen_size_) {
            // Source shrank (clear / set_content rollback) — reset.
            last_seen_size_ = source_.size();
            last_grow_ms_   = ms_total;
            reveal_cp_      = 0.0;
            reveal_ms_      = ms_total;
        }

        // ── Advance the continuous reveal cursor ──
        //
        // Total codepoints currently available (whole source). The cursor
        // eases toward this at floor + proportional catch-up, integrated
        // over real elapsed time so its velocity is smooth no matter how
        // bytes were batched onto the wire. This is the typewriter, owned
        // entirely by the widget.
        const std::size_t total_cp = [&] {
            std::size_t n = 0;
            for (std::size_t i = 0; i < source_.size(); ++i)
                if ((static_cast<unsigned char>(source_[i]) & 0xC0) != 0x80) ++n;
            return n;
        }();
        if (reveal_ms_ == 0) { reveal_ms_ = ms_total; reveal_cp_ = 0.0; }
        {
            double elapsed_s = (ms_total - reveal_ms_) / 1000.0;
            if (elapsed_s < 0.0)   elapsed_s = 0.0;
            // Bound the per-frame advance so a frame served VERY late (the
            // terminal was backgrounded for seconds, a giant relayout spiked
            // render cost) can't integrate the whole gap and teleport the
            // cursor to the live edge in one jump — a visible burst. But the
            // clamp must be GENEROUS enough to absorb the ordinary slow-frame
            // case: on a non-sync terminal the loop wakes on the 100 ms Tick
            // between RAF beats, and any single render frame on a long thread
            // can land 80-120 ms after the previous one. A 33 ms clamp turned
            // every such frame into a permanent deficit — the cursor advanced
            // 33 ms of progress for 100 ms of wall-clock, fell progressively
            // behind the wire, and the unrevealed backlog only flushed at
            // settle (the "stuck then burst" the reveal exists to prevent).
            // Clamp at 120 ms instead: it covers one Tick period plus margin,
            // so a normally-late frame catches the cursor up cleanly, while a
            // pathological multi-second gap still steps at most ~120 ms worth
            // (a fraction of a viewport at the ceiling cps below) — a small
            // catch-up glide, never a teleport.
            if (elapsed_s > 0.120) elapsed_s = 0.120;
            reveal_ms_ = ms_total;
            const double backlog = static_cast<double>(total_cp) - reveal_cp_;
            if (backlog <= 0.0) {
                reveal_cp_ = static_cast<double>(total_cp);
            } else {
                // Time-bounded typewriter. Two terms, take the larger:
                //
                //   • FLOOR (200 cps) — the steady cadence for ordinary
                //     drip streaming, so a small backlog reveals at a calm,
                //     even pace and reads like a typewriter, not a sweep.
                //
                //   • DRAIN — reveal whatever backlog exists within a fixed
                //     wall-clock window (kDrainSecs), so the reveal time is
                //     bounded by TIME, not by a fixed char/sec ceiling. This
                //     is the load-bearing term against the real wire: the
                //     model stalls for multiple seconds, then the edge
                //     delivers a whole paragraph in a single burst (measured:
                //     up to ~2000 chars landing in one millisecond after a
                //     10 s+ stall, with more bursts following every ~13 s). A
                //     fixed cps ceiling let that backlog COMPOUND across
                //     successive bursts — each one revealed slower than the
                //     next arrived — so the cursor fell ever further behind
                //     and the unrevealed tail dumped at settle (the
                //     stuck-then-burst the user sees). Sizing the rate as
                //     backlog / kDrainSecs caps the worst-case reveal time at
                //     kDrainSecs no matter how big the burst, so backlog can
                //     never accumulate beyond one window's worth: a 2000-char
                //     dump glides out in ~2 s, a 250-char drip stays on the
                //     200 floor (~1 s). No arbitrary cps ceiling to out-pace.
                //
                //     kDrainSecs is the load-bearing tunable. It must be SMALL
                //     enough that the cursor is caught up (or nearly) by the
                //     time the turn SETTLES — at settle the host drops live_
                //     and the overlay returns the full text at once, so any
                //     backlog still unrevealed at that instant dumps in one
                //     frame (the real "suddenly renders a lot" the user sees,
                //     distinct from byte arrival). 0.5 s keeps the worst-case
                //     backlog reveal under ~2 s so a turn that ends shortly
                //     after its last burst finds little or nothing to dump,
                //     while staying slow enough to read as a typewriter, not
                //     a teleport.
                constexpr double kFloorCps  = 200.0;  // steady typewriter cadence
                constexpr double kDrainSecs = 0.5;    // worst-case backlog reveal time
                double cps = backlog / kDrainSecs;
                if (cps < kFloorCps) cps = kFloorCps;
                // Finalize ramp: when a deadline is pending, force a
                // rate that REACHES the live edge by the deadline. We
                // take the max with the normal drain so a small backlog
                // already gliding faster than the ramp keeps its calm
                // typewriter cadence — the ramp only KICKS IN to prevent
                // the cursor finishing late. The deadline + start_ms
                // pair makes this immune to dropped frames: ramp_cps is
                // computed from time STILL REMAINING, so however many
                // frames the loop misses, the cursor still lands on the
                // edge at the deadline.
                if (finalize_deadline_ms_ != 0) {
                    const std::int64_t remaining_ms =
                        finalize_deadline_ms_ - ms_total;
                    if (remaining_ms <= 0) {
                        // Past deadline — snap to edge this frame.
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
        // Ramp completion: cursor reached the edge. Flip live_ off so
        // the next frame short-circuits at the !live_ guard above and
        // returns the settled cached_build_ with no visible pop — the
        // reveal cursor is already at the live edge, so the overlay
        // and the settled build are equivalent for the trailing chars.
        // Clear the deadline so a future stream can request a new ramp.
        //
        // Critical: request one more animation frame here so the host's
        // finish() (gated on !is_finalizing()) runs on the very next
        // build(). Without this RAF kick, ramp completion clears
        // is_finalizing AND drops live_ in the same instant, the host's
        // RAF gate (which armed on is_finalizing) goes idle, and the
        // visual hash stops advancing — so the host never re-enters
        // view() to call finish(). The currently-rendered cached_build_
        // was assembled mid-stream with the trailing block possibly
        // uncommitted (open fence, in-progress paragraph), and that
        // intermediate render would stay frozen on screen indefinitely.
        // One forced frame closes the gap: next render calls finish(),
        // commits the tail, rebuilds cached_build_, returns the settled
        // tree, RAF lapses naturally because nothing is animating.
        if (finalize_deadline_ms_ != 0
            && reveal_cp_ >= static_cast<double>(total_cp))
        {
            finalize_deadline_ms_ = 0;
            live_ = false;
            request_animation_frame();
            return cached_build_;
        }
        const std::size_t revealed_cp =
            static_cast<std::size_t>(reveal_cp_);

        // age_at_tail_ms is now the time since the CURSOR last moved past
        // a fresh codepoint, not since bytes arrived — so it never snaps
        // to 0 on a burst. When the cursor is at the edge (caught up) it
        // grows like last_grow; while catching up it stays ~0 (actively
        // revealing).
        const std::int64_t age_at_tail_ms =
            (reveal_cp_ >= static_cast<double>(total_cp))
                ? (ms_total - last_grow_ms_)
                : 0;

        // Walk a copy of cached_build_ down its rightmost spine to
        // the last leaf TextElement.
        auto find_last_text = [](Element& root) -> TextElement* {
            Element* cur = &root;
            for (;;) {
                if (auto* t = std::get_if<TextElement>(&cur->inner)) {
                    return t->content.empty() ? nullptr : t;
                }
                if (auto* b = std::get_if<BoxElement>(&cur->inner)) {
                    if (b->children.empty()) return nullptr;
                    cur = &b->children.back();
                    continue;
                }
                return nullptr;
            }
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
        constexpr std::size_t kTrailLen    = 24;   // codepoints in gradient
        constexpr std::size_t kScrambleLen = 4;    // codepoints scrambled
        constexpr std::int64_t kScrambleMs = 180;  // scramble → resolve
        constexpr std::int64_t kCharStepMs = 28;   // per-char age delta

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
            // ── Apply the reveal cursor: hide the unrevealed tail ──
            // The committed prefix (everything but this last TextElement)
            // is already settled and always shown. Only the live tail's
            // trailing edge is paced: chop off the codepoints the cursor
            // hasn't reached yet so text unfolds at the cursor's smooth
            // velocity instead of popping in with each wire burst. The
            // unrevealed bytes stay in source_ — they reappear next frame
            // as the cursor advances. Bounded: we only ever hide what's
            // beyond the cursor, and the cursor monotonically approaches
            // total_cp, so nothing is lost.
            if (revealed_cp < total_cp) {
                const std::size_t hidden_cp = total_cp - revealed_cp;
                // Hide at most the tail's own codepoints (never cut into
                // the committed prefix — it's not in this TextElement).
                const std::size_t cut_start =
                    utf8_step_back(tail->content, hidden_cp);
                if (cut_start < tail->content.size()) {
                    tail->content.resize(cut_start);
                    // Drop runs past the cut; clip the straddling one.
                    std::vector<StyledRun> kept;
                    kept.reserve(tail->runs.size());
                    for (const auto& r : tail->runs) {
                        if (r.byte_offset >= cut_start) continue;
                        const std::size_t end = r.byte_offset + r.byte_length;
                        kept.push_back(StyledRun{
                            .byte_offset = r.byte_offset,
                            .byte_length = std::min(end, cut_start) - r.byte_offset,
                            .style       = r.style,
                        });
                    }
                    tail->runs = std::move(kept);
                    tail->cached_width = -1;   // content shrank — re-wrap
                }
            }

            const std::string_view orig = tail->content;

            // Find the trail window WITHOUT scanning the entire
            // content. Walk backward from the end byte-by-byte until
            // we've passed kTrailLen codepoints (a UTF-8 codepoint
            // start is any byte whose top 2 bits are not 10). Then
            // index codepoints only within [trail_byte_start, end).
            // O(kTrailLen) per frame, independent of tail length.
            const std::size_t trail_byte_start =
                utf8_step_back(orig, kTrailLen);
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
                Style s;
                if (scrambling) {
                    const bool flick =
                        ((ms_total / 60 + k) & 1) == 0;
                    s = Style{}
                        .with_fg(flick
                            ? Color::rgb(255,  80, 180)
                            : Color::rgb(255, 160,  60))
                        .with_bold();
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
            // Force re-wrap on the copy ONLY when scramble is active.
            // Scramble may substitute a 1-byte ASCII for a 3-byte box
            // glyph, changing content byte length and invalidating the
            // wrap cache's content_size key. When no scramble is
            // active (age_at_tail >= kScrambleMs across the entire
            // window) the trail bytes are byte-identical to the
            // original tail — the wrap cache from the source render is
            // still valid and re-wrapping would be wasted O(content)
            // work on every animation frame. On a multi-KB code-block
            // tail this avoidance is the difference between smooth
            // and chokes-the-renderer.
            const bool scramble_active =
                age_at_tail_ms < kScrambleMs +
                static_cast<std::int64_t>(scramble_n) * kCharStepMs;
            if (scramble_active) {
                tail->cached_width = -1;
            }
        }
        trail_done:;

        // ── Inline caret ──
        //
        // PULSE the last codepoint of the tail in place — recolor it
        // with a cycling magenta→cyan fg + a faint block background so
        // the streaming edge reads as a live caret — instead of
        // APPENDING a block glyph after it. Appending added one display
        // column to the last wrapped row; whenever that row was already
        // at the wrap width the extra column spilled to a NEW wrapped
        // row. That row existed only while live_ was true, so the
        // instant the turn settled (live_=false → cached_build_, no
        // caret) the row vanished and everything below — the composer
        // and status bar — jumped up by one row. Intermittent because
        // it only triggered when the final line happened to land at the
        // column boundary (the "sometimes the chrome shifts up at turn
        // end" symptom). Recoloring in place is byte-width-identical to
        // cached_build_'s last row, so the live↔settled height is always
        // equal and the seam can never shift.
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
