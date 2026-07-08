#pragma once
// maya::widget::markdown — Full-featured terminal markdown rendering
//
// Parses CommonMark + GFM markdown and converts to maya Element trees.
// Supports: headings (ATX + setext), bold, italic, bold+italic, inline code,
// fenced + indented code blocks, blockquotes (incl. GitHub alerts), nested
// lists (ordered/unordered), task lists, tables, links (inline, reference,
// collapsed, shortcut), images (incl. reference), footnotes (multi-paragraph,
// code), strikethrough, highlight (== ==), subscript (~x~) / superscript
// (^x^), horizontal rules, backslash escapes, autolinks, bare URLs, email
// autolinks, hard line breaks, HTML entities, emoji shortcodes, definition
// lists, an HTML subset (<br>, <kbd>, <mark>, <sub>, <sup>, <abbr>,
// <strong>/<em>/<span>, <details>/<summary>, <a id="…">), and GitHub-style
// @user / #123 / org/repo#42 references.
//
// Usage:
//   auto ui = markdown("## Hello\nThis is **bold** and `code`.");

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "../core/tracked.hpp"
#include "../core/animation.hpp"   // anim::RateCursor (reveal typewriter)
#include "../element/builder.hpp"
#include "../text/stream_sink.hpp"
#include "markdown/ast.hpp"

namespace maya {

// ============================================================================
// Parser
// ============================================================================

[[nodiscard]] md::Document parse_markdown(std::string_view source);

// ============================================================================
// AST to Element conversion
// ============================================================================

[[nodiscard]] Element md_inline_to_element(const md::Inline& span);
[[nodiscard]] Element md_block_to_element(const md::Block& block);

// ============================================================================
// Public API
// ============================================================================

/// Parse markdown and return an Element tree.
[[nodiscard]] Element markdown(std::string_view source);

// ── Themable palette ────────────────────────────────────────────────────────
// Every colour slot the markdown renderer uses. Defaults match the built-in
// look; set once at startup to make markdown cohere with your app's theme:
//
//   auto p = maya::default_markdown_palette();
//   p.code_fg = my_code_color;  p.link_fg = my_link_color;
//   maya::set_markdown_palette(p);
//
// Single-threaded: call before the UI loop starts. The renderer reads the
// active palette live (no per-call argument threading).
struct MarkdownPalette {
    Color text, heading1, heading2, heading3, heading_dim, heading_rule;
    Color bold_fg, italic_fg, code_fg, code_bg, link_fg, image_fg, strike_fg;
    Color quote_bar, quote_text, list_bullet, list_num, checkbox_fg, checkbox_off;
    Color code_border, code_lang, hrule_fg, footnote_fg, table_border, table_header;
    Color highlight_bg, highlight_fg, mention_fg, kbd_fg, kbd_border;
    Color alert_note, alert_tip, alert_important, alert_warning, alert_caution;
};

/// The renderer's current palette (defaults at startup).
[[nodiscard]] MarkdownPalette default_markdown_palette();
/// Overwrite the active markdown palette.
void set_markdown_palette(const MarkdownPalette& p);

// ============================================================================
// StreamingMarkdown — Progressive monotonic rendering for streaming text
// ============================================================================
//
// Two-tier rendering — the strong invariant that makes this widget
// impossible to ghost regardless of how the input is chunked:
//
//   1. **Committed prefix.**  Every byte before the last structural
//      boundary (blank line, closing code fence, end of heading) has
//      been parsed as full markdown and cached as Elements.  These
//      blocks are never re-rendered or re-classified.
//
//   2. **Opaque tail.**  Bytes since the last boundary are rendered as
//      a single in-progress paragraph with INLINE-only markdown (bold,
//      italic, code spans, links).  Block markers at line starts
//      (`|`, `-`, `*`, `>`, `#`, `\`\`\``) render as literal characters
//      until the boundary detector commits them, at which point the
//      whole block snaps into its formatted form in the committed prefix.
//
//   3. **Byte integrity.**  All input flows through an internal
//      maya::StreamSink — split UTF-8 codepoints and half-written ANSI
//      escapes are buffered until their continuation arrives, so the
//      cell grid never sees a partially-decoded glyph.
//
// Together these three invariants guarantee that the element-tree
// height is a monotonic function of the stream's byte position:
// appending bytes can only extend the rendered output, never reflow
// or shrink it.  The renderer's row diff therefore never has to chase
// retroactive height changes — there is no ghosting class to fight.
//
// Usage:
//   StreamingMarkdown md;
//   md.feed("# Hello\n\nSome **bold");
//   md.feed(" text**\n\nA tabl");
//   md.feed("e:\n| a | b |\n|---|---|\n| 1 | 2 |\n\n");
//   auto ui = md.build();   // committed: heading, paragraph, table
//   md.finish();             // commit any pending tail at end-of-stream
//
// Pre-existing `append()` / `set_content()` continue to work and route
// through the same path — feed() is the recommended name for new code.

class StreamingMarkdown {
    // ── Committed-prefix snapshot ──
    //
    // The settled blocks live behind a shared_ptr so we can capture them
    // by value into the prefix ComponentElement's render() lambda
    // without copying — and so the renderer's cross-frame
    // component_cache (keyed by ComponentElement* + width) can amortise
    // layout + paint of every settled block down to one call per
    // (instance, width) pair instead of N children walked every frame.
    //
    // commit_range pushes new blocks here and bumps `generation`. The
    // ComponentElement instance held inside cached_build_ stays alive
    // (and at a stable address) across frames as long as the generation
    // hasn't moved, which is the property the component_cache needs to
    // hit. When the generation does move, build() rebuilds the
    // ComponentElement (giving it a new address) so the cache cleanly
    // misses, re-renders, and stores the new entry.
    //
    // `blocks` holds `shared_ptr<const Element>` so each rendered block
    // is heap-allocated with a stable address. Reasons:
    //   1. push_back is a pointer copy (16 B) instead of a variant
    //      move-construct that drags the per-block BoxElement body
    //      around. On a 1000-paragraph commit history that's the
    //      difference between a few hundred bytes of memmove and a
    //      few hundred KB.
    //   2. Vector reallocation when capacity grows only moves
    //      shared_ptrs — the Element trees themselves don't move, so
    //      anything else holding pointers into the prefix (a future
    //      per-block layout cache, debug tooling) sees stable
    //      addresses.
    //   3. `const` on the pointed-to Element is the structural promise
    //      that committed blocks are immutable — the next prefix
    //      generation gets a fresh shared_ptr, never an in-place edit.
public:
    /// Discriminator for a committed block's *kind*. Matches the
    /// active variant of md::Block 1:1 but stays an opaque enum so the
    /// header doesn't drag in the full md::Block visitor surface.
    /// Used by host-side UIs (fold pickers, find, copy-block) that
    /// want to filter by block type without re-parsing.
    enum class BlockKind : std::uint8_t {
        Paragraph, Heading, CodeBlock, Blockquote, List, HRule,
        Table, FootnoteDef, Alert, DefList, Details, HtmlBlock,
        Other = 0xFF,
    };

    /// Per-block metadata, parallel to CommittedPrefix::blocks. The
    /// source byte range is the *primary key* for any UI state that
    /// must survive a reparse — fold, scroll-offset-inside-code-block,
    /// "this block is highlighted" — exactly the Zed pattern.
    struct BlockMeta {
        std::size_t  source_offset = 0;  ///< byte index of the block's first byte in source_
        std::size_t  source_end    = 0;  ///< exclusive end
        BlockKind    kind          = BlockKind::Other;
        std::uint16_t line_count   = 0;  ///< body line count (display hint for fold stub)
        std::string  lang;               ///< only set for CodeBlock
    };

private:
    struct CommittedPrefix {
        std::vector<std::shared_ptr<const Element>> blocks;
        std::vector<BlockMeta>                      metas;   ///< parallel to blocks
        std::uint64_t                                generation = 0;
    };
    mutable std::shared_ptr<CommittedPrefix> prefix_ =
        std::make_shared<CommittedPrefix>();

    // Per-instance discriminator stamped at construction and rotated
    // on clear(). Mixed into the prefix ComponentElement's hash_id
    // (markdown.cpp build()) so two StreamingMarkdown widgets at the
    // same `prefix_->generation` don't collide on the renderer's
    // hash-keyed cache. Each agentty turn owns its own
    // StreamingMarkdown via view_cache.message_md, and generation
    // counters start at 0 in every instance — without a per-instance
    // mixin, the hash for ("strmd-prefix", generation=N) would alias
    // turn N's prefix cells onto turn M's region the moment both have
    // hit N commits. Symptom: prefix text vanishes (last paint wins),
    // scrollback rows commit blank (cells_rows < content_h) when the
    // overflow `\r\n` scrolls them off the viewport top. Same shape
    // as the bug the Element(shared_ptr) ctor solves via
    // id_for_shared.
    //
    // Rotated on clear() so a logical reset (same widget instance,
    // new stream) can't alias the pre-clear prefix cells onto the
    // post-clear prefix region. Without the rotation, the first
    // post-clear commit produces generation=1 and a hash matching
    // the (still-present, retained for ~2 seconds in entries_by_hash)
    // entry from the pre-clear stream's generation=1 — blits the old
    // bitmap at the new region's coordinates.
    std::uint64_t instance_id_ = detail::next_component_generation();

    std::string source_;                // codepoint-safe accumulated bytes
    size_t committed_ = 0;              // bytes parsed into finalized blocks

    // Monotonic version counter — bumped whenever source_ or committed_
    // changes in a way that could affect render_tail's output or the
    // inline-tail cache's stable-prefix slice. Lets the per-frame
    // build() / render_tail() short-circuits do O(1) compares against
    // their cached version instead of O(tail) FNV-1a hashes on every
    // frame. The hash defended against a phantom case (build_dirty_
    // flipped but tail bytes round-tripped) that the call-graph never
    // actually produces — every mutator that flips build_dirty_ also
    // mutates bytes the tail depends on, so a single producer-side
    // bump captures the invariant precisely.
    //
    // Wrap-around: at 30 fps × one bump/frame this would take ~19
    // billion years to overflow a uint64_t — comfortably never.
    std::uint64_t source_version_ = 0;
    bool in_code_fence_ = false;        // ``` fence state at committed_
                                        // (used by render_tail to know
                                        // whether the in-progress tail
                                        // starts inside an open fence)

    // Host-set: "more bytes are coming". When true, build() wraps
    // its output in a ComponentElement that re-renders on every
    // animation frame so the trailing cursor caret blinks. Cleared
    // by finish() (stream done) and by clear() (logical reset).
    // Mutable: render_live_overlay_() (called from the const build()
    // path) auto-flips this to false when a finalize ramp completes,
    // so the next frame returns the settled cached_build_ — the
    // widget owns the final live→settled transition end-to-end.
    //
    // Tracked<>: every write auto-bumps build_dirty_ via InvalidateBuild.
    // The has_tail shape in build.cpp depends on live_ (a reserved empty
    // trailing slot exists only while live_), so any live→settled flip
    // requires a cache rebuild. Wrapping live_ in Tracked makes the
    // coupling structural — you cannot assign to live_ without bumping
    // build_dirty_, by construction. The prior naked `bool live_` + manual
    // `build_dirty_ = true` pairing was the empty-trailing-box bug source.
    struct InvalidateBuild {
        void operator()(StreamingMarkdown& o) const noexcept {
            o.build_dirty_ = true;
        }
    };
    mutable Tracked<bool, StreamingMarkdown, InvalidateBuild> live_{*this, false};

    // Host-set: opt INTO the animated streaming-reveal effect (the
    // hot→cool gradient trail, the matrix-style scramble→resolve on
    // the newest glyphs, and the pulsing block caret). DEFAULT OFF.
    //
    // Why off by default: those effects re-color and re-glyph the
    // trailing ~24 codepoints on EVERY animation frame. On terminals
    // that honor synchronized output the swap is atomic but the colors
    // still visibly sweep; on terminals that don't, it's outright
    // flicker. Either way the eye reads "the text I just received is
    // shimmering," which most hosts do not want. When off, build()
    // returns the settled, fully-styled tail immediately — text simply
    // appears as it streams (the host's own typewriter pacing still
    // applies), with no per-frame churn and no animation-frame
    // requests. Hosts that specifically want the effect call
    // set_reveal_fx(true).
    bool reveal_fx_ = false;

    // Reveal cursor pacing. The typewriter advances at
    //   cps = max(floor_cps, backlog / drain_secs)
    // so a quiet stream still types at floor_cps, while a large
    // burst is drained within ~drain_secs. Defaults: 120 cps floor,
    // 0.8 s drain — fast enough to keep up with realistic model
    // output, slow enough that the scramble / sweep cursor stay
    // visible. Tunable so dev tools / tests can crank it WAY down
    // (e.g. 20 cps) to make the animation unmissable when validating
    // visually against recorded streams.
    double reveal_floor_cps_  = 120.0;
    double reveal_drain_secs_ = 0.8;

    // ── Per-build size tracking for age-based animation ──
    //
    // Each time build() runs and the source has grown since the
    // previous build, we stamp `last_grow_ms_` with the current
    // monotonic time. The finalize() lambda compares the current
    // time against this stamp to age out the trailing-edge effects
    // (scramble → resolve, hot → cool color trail). Without this we
    // can only animate based on absolute time, which gives every
    // visible char the same animation phase — the eye reads that as
    // "some lights are flashing", not "text is appearing".
    //
    // The tracking is approximate: we don't time-stamp individual
    // bytes (too much state), we time-stamp the "trailing batch" as
    // a whole and assume the model emits at a roughly steady rate.
    // Combined with the typewriter reveal in moha (which feeds bytes
    // at a fixed rate of ~220 chars/sec), this gives each char an
    // age that's accurate to within a frame.
    mutable std::size_t last_seen_size_ = 0;
    mutable std::int64_t last_grow_ms_   = 0;

    // ── Continuous reveal cursor (maya-owned typewriter) ──
    //
    // The host feeds the FULL set of arrived bytes every frame and does
    // no pacing. To make the live edge advance smoothly regardless of how
    // the wire batches bytes (Anthropic ships 50-100 chars per delta with
    // 90-200 ms gaps), the widget runs its own reveal cursor: a fractional
    // codepoint count that eases toward the total at a constant-velocity
    // catch-up integrated over real elapsed time. Because the cursor moves
    // on its OWN clock — not on byte arrival — its velocity never snaps,
    // so the scramble/gradient/caret it drives stay continuous across
    // bursts and pauses alike. `reveal_cp_` is codepoints revealed;
    // `reveal_ms_` is the wall-clock of the last advance. The trailing-edge
    // age is then measured from the cursor, giving each char an age that
    // increases monotonically as the cursor passes it.
    mutable double       reveal_cp_  = 0.0;
    mutable std::int64_t reveal_ms_  = 0;

    // Central typewriter integrator. reveal_cp_ above is the public face
    // of the reveal cursor (read by reveal_in_progress(), the byte clip,
    // the overlay); reveal_rate_cursor_ is the maya::anim::RateCursor that
    // actually integrates it when MAYA_REVEAL_CENTRAL_CURSOR is enabled
    // (the default). The cursor is kept in lock-step with reveal_cp_ each
    // frame so the rest of the widget reads a single source of truth. The
    // RateCursor is a rate-smoothed bounded-lag glide (it reveals at
    // backlog / drain_secs so it tracks the model's own speed, low-passed
    // so a chunky wire slides in instead of teleporting) — see
    // animation.hpp; tests in test_animation.cpp + reveal_pacing_test.cpp.
    // The host overrides the seed pacing below via set_reveal_pacing each
    // frame, so these constructor args are only the cold-start default.
#ifndef MAYA_REVEAL_CENTRAL_CURSOR
#define MAYA_REVEAL_CENTRAL_CURSOR 1
#endif
    mutable ::maya::anim::RateCursor reveal_rate_cursor_{120.0, 0.8};

    // Cursor advance scratch — written by advance_reveal_cursor_()
    // (called from build() top), consumed by render_live_overlay_()
    // for visual decoration. Avoids re-walking source_ for total_cp
    // and re-reading the clock in overlay. SIZE_MAX as a sentinel for
    // "advance hasn't run this frame" — overlay then derives them on
    // the fly (defensive, shouldn't happen in practice).
    mutable std::int64_t cursor_advance_ms_total_   = 0;
    mutable std::size_t  cursor_advance_total_cp_   = 0;
    mutable std::int64_t cursor_advance_age_tail_ms_ = 0;

    // Wall-clock stamp of the FIRST frame on which the reveal cursor
    // reached the live edge (reveal_cp_ >= total_cp). Reset to 0 whenever
    // the source grows or the cursor falls behind the edge again. This is
    // the clock that the finalize ramp-completion gate keys its
    // scramble-settle window off — NOT last_grow_ms_.
    //
    // Why a separate stamp: the scramble at the trailing edge is anchored
    // to the CURSOR position (decorate_text_reveal scrambles the
    // codepoints in [unrevealed_cp, unrevealed_cp + kScrambleLen)), so a
    // codepoint is freshly "typed" — and scrambling — the moment the
    // CURSOR sweeps over it, regardless of how long ago its bytes
    // arrived. On a long message whose bytes all landed seconds ago, the
    // finalize ramp drives the cursor across a large backlog FAST; when
    // it lands on the edge the tail glyphs the sweep just exposed are
    // still mid-scramble even though last_grow_ms_ is far in the past.
    // Gating live_=false on (ms_total - last_grow_ms_) would pass
    // immediately and freeze that scramble garbage onto the settled
    // tail. Gating on (ms_total - reveal_edge_reached_ms_) instead
    // guarantees the cursor-swept tail has the full scramble window to
    // resolve to clean glyphs before the live→settled handoff.
    mutable std::int64_t reveal_edge_reached_ms_ = 0;

    // Cached codepoint counts: total_cp counts cp in source_[0..N],
    // committed_cp counts cp in source_[0..committed_]. Both are
    // monotone-extend over source_ (committed_ only advances; source_
    // only grows, except on clear() which resets both caches via
    // size-shrink detection). Cached so advance_reveal_cursor_()
    // doesn't walk the entire source byte-by-byte every frame at 60 fps
    // for large messages — the walk was O(source_size) per frame, i.e.
    // 50 KB × 60 fps = 3 MB/s of pure UTF-8 lead-byte scanning per
    // streaming message. Cache is updated incrementally by counting only
    // new bytes since the last cached offset.
    mutable std::size_t cached_total_cp_       = 0;
    mutable std::size_t cached_total_cp_at_    = 0;  // source_.size() when cached
    mutable std::size_t cached_committed_cp_   = 0;
    mutable std::size_t cached_committed_cp_at_ = 0; // committed_ when cached

    // Byte offset corresponding to reveal_cp_ (rounded down to a UTF-8
    // codepoint boundary), refreshed each frame by
    // advance_reveal_cursor_(). When reveal_fx_ && live_, this is the
    // "visible bytes" clip — find_block_boundary / commit_range / the
    // tail extraction in build() all clamp to it so eager-styled blocks
    // (H2 underline, code-fence chrome, table rules) don't paint ahead
    // of the typewriter cursor. When reveal_fx_ is off OR not live, the
    // clip is set to source_.size() (no-op gating). SIZE_MAX before the
    // first advance_reveal_cursor_ call this stream.
    mutable std::size_t reveal_byte_clip_ = static_cast<std::size_t>(-1);

    // Translate a codepoint count into a byte offset within source_,
    // walking UTF-8 lead bytes from 0. Clamps to source_.size() if
    // n_cp >= total codepoints. Caches its last result so successive
    // monotone calls (cursor advance, then append_safe in the same
    // frame) avoid the second walk.
    [[nodiscard]] std::size_t byte_offset_for_cp(std::size_t n_cp) const noexcept;
    mutable std::size_t cp_to_byte_cache_cp_   = 0;
    mutable std::size_t cp_to_byte_cache_byte_ = 0;
    mutable std::size_t cp_to_byte_cache_at_   = 0; // source_.size() at cache time

    // ── Finalize ramp ──
    //
    // request_finalize(ramp_ms) records a deadline by which the reveal
    // cursor MUST reach the live edge, then the widget flips live_=false
    // itself. While the ramp is active the cursor advance picks the FASTER
    // of the normal drain rate and the ramp rate (remaining_cp / time_left)
    // — never slower than the steady reveal, so a small backlog still
    // types out at typewriter cadence, but a large backlog is guaranteed
    // to finish within ramp_ms instead of glitching at settle. The host
    // calls this once when its turn settles; if the cursor is already at
    // the edge the widget flips live_=false on the very next build().
    //
    // finalize_deadline_ms_ == 0 means no ramp in flight.
    mutable std::int64_t finalize_deadline_ms_ = 0;

    // Animation throttle: bucket the wall clock into ~33 ms phases
    // and only request the next animation frame when the phase
    // actually advances. Without this RAF fires every 16 ms and the
    // composer below the live tail repaints at 60 Hz even though the
    // scramble/caret/gradient visibly steps at ~30 Hz at most.
    mutable std::int64_t last_anim_phase_ = -1;

    // ── Resumable boundary scanner ──
    //
    // Earlier `find_block_boundary` rescanned [committed_, source_.size())
    // on every call. With the view layer calling build() once per frame
    // and append() landing a few bytes between frames, that meant each
    // frame paid O(committed_to_eos) just to relocate the next block
    // separator — the per-frame O(source_) tax that made late tokens
    // in long replies feel sluggish (50 KB transcript → 50 KB scan
    // every frame; ~30 fps × 50 KB = 1.5 MB/s of pure boundary scan
    // for one streaming message).
    //
    // The scan is monotonic: every byte the scanner has already visited
    // tells us nothing new on a re-run. So persist the cursor + the
    // fence parity at the cursor + the best boundary found so far, and
    // resume on the next call. New work per call is O(bytes added since
    // last call), matching the actual incoming token rate.
    //
    // Invariants:
    //   • scan_cursor_ ≥ committed_ at all times (commit_range only
    //     advances committed_ up to a boundary the scanner already
    //     returned, never past scan_cursor_).
    //   • scan_in_fence_ is the ``` parity at scan_cursor_ specifically.
    //     `in_code_fence_` above is the parity at committed_; the two
    //     can diverge because commit_range advances committed_ to a
    //     scanner-found boundary while scan_cursor_ may already be
    //     deeper.
    //   • scan_last_boundary_ ≥ committed_; reset to committed_ inside
    //     commit_range so a "consumed" boundary doesn't get returned
    //     again on the next find call.
    size_t scan_cursor_         = 0;
    bool   scan_in_fence_       = false;
    size_t scan_last_boundary_  = 0;

    // ── Pending-boundary ledger ──
    // Every block boundary the scanner has discovered PAST committed_,
    // in increasing order. find_block_boundary returns only the LATEST
    // boundary, but the reveal-paced commit gate (append_safe /
    // commit_revealed_) needs the largest boundary ≤ the reveal cursor —
    // an INTERMEDIATE one on a multi-block tail. Without the ledger the
    // gate degenerates to all-or-nothing: the latest boundary rides the
    // live edge (always ahead of the cursor, which lags ~drain_secs by
    // design), so nothing ever commits mid-stream and the tail grows
    // O(turn) — re-introducing the long-turn render lag. Entries are
    // pruned to > committed_ inside commit_range; cleared by clear().
    std::vector<size_t> scan_boundaries_;

    // Codepoint / escape-sequence safety net.  Every byte the user feeds
    // passes through here before being appended to source_, so source_ is
    // always a valid UTF-8 string with no half-written ANSI sequences —
    // even if the caller chunks input mid-codepoint or mid-CSI.
    StreamSink sink_;

    // Ref-defs accumulated across every committed parse.  The tail parse
    // reuses this map so links pointing at earlier-committed `[label]: url`
    // still resolve, and so we avoid re-running collect_ref_defs() on the
    // tail every frame.
    std::unordered_map<std::string, md::LinkRef> ref_defs_;

    // ── per-frame cache ────────────────────────────────────────────────
    // build() is called every frame by the view layer. When neither source_
    // nor committed_ has moved since the last build, we return the cached
    // Element directly — no parse, no assembly.  Any mutator (feed /
    // append / set_content / finish / clear) bumps `build_dirty_`.
    mutable Element cached_build_;
    // Live-mode wrapper. When live_ is true, build() returns this
    // instead of cached_build_; it’s a vstack of [cached_build_,
    // cursor_row] rebuilt every frame and requests an animation
    // frame so the cursor blinks. The committed-block children are reused
    // in place across frames (only the trailing tail child is refreshed +
    // decorated) when live_overlay_prefix_gen_ still matches
    // prefix_->generation, so the per-frame overlay cost is O(tail), not
    // O(committed block count). cached_build_ is a vstack of one direct
    // ComponentElement per committed block plus the tail leaf; copying that
    // whole children vector every animation frame was the residual
    // long-turn streaming cost after the commit fix.
    mutable Element cached_live_;
    // prefix_->generation that cached_live_'s committed children mirror.
    // (uint64_t)-1 forces a full rebuild on the first overlay frame.
    mutable std::uint64_t live_overlay_prefix_gen_ =
        static_cast<std::uint64_t>(-1);

    // ── Idle-overlay memo ──────────────────────────────────────────────
    // render_live_overlay_ runs every animation frame while live_, but its
    // output cached_live_ is a pure function of the fields below. During the
    // long idle-but-live stretch after the reveal catches up to the edge and
    // the trail cools, only the caret pulse moves — bucketed coarse here so
    // most idle frames share a signature and return cached_live_ untouched,
    // skipping the O(blocks) spine walk + O(tail) decorate re-slice that was
    // the per-frame floor a long live turn paid between deltas. See the memo
    // block in render_live_overlay_ for the full rationale.
    struct OverlaySig {
        std::uint64_t prefix_gen   = 0;
        std::uint64_t src_version  = 0;
        std::size_t   revealed_cp  = 0;
        std::size_t   total_cp     = 0;
        std::size_t   byte_clip    = 0;
        std::int64_t  age_term     = 0;
        std::int64_t  caret_bucket = 0;
        bool operator==(const OverlaySig&) const = default;
    };
    mutable OverlaySig overlay_sig_{};
    mutable bool       overlay_sig_valid_ = false;
    mutable bool    build_dirty_  = true;
    mutable size_t  cached_tail_size_ = 0;   // tail length when cache was built
    // Generation reflected in cached_build_'s prefix component slot —
    // used to short-circuit the rebuild when only the tail changed.
    mutable std::uint64_t cached_prefix_gen_ = 0;
    // Generation reflected in cached_build_'s rendered prefix. When
    // folds_ changes (toggle_fold / set_fold / unfold_all) we bump
    // fold_generation_; the cache's fold_gen lagging means the
    // prefix component lambda baked in a stale fold snapshot and
    // must be rebuilt.
    mutable std::uint64_t cached_fold_gen_   = 0;
    // Whether cached_build_ currently holds a tail child slot. If this
    // flips between frames the cache can't be mutated in place; a full
    // rebuild is needed to reshape the outer vstack.
    mutable bool          cached_has_tail_   = false;
    // Whether cached_build_ currently holds a prefix child slot. Same
    // reason — flipping this requires a structural rebuild.
    mutable bool          cached_has_prefix_ = false;
    // Committed-prefix window state reflected in cached_build_. The
    // committed blocks are emitted as [head_wrapper?(covers [0, lo)),
    // block_lo .. block_{N-1}] so the OUTER renderer walks only O(window)
    // children per frame instead of O(N). cached_block_count_ is N (the
    // committed block count baked into the cache); cached_window_lo_ is
    // the window base lo. The commit-append fast path uses these to tell
    // a same-window pure growth (lo unchanged → keep the head + old
    // window blocks, append the new ones) from a window SLIDE (lo
    // advanced across a chunk boundary → the head wrapper's hash changed
    // and some formerly-individual blocks fold into it, forcing a full
    // children re-emit).
    mutable std::size_t   cached_block_count_ = 0;
    mutable std::size_t   cached_window_lo_   = 0;
    // 64-bit FNV-1a hash of the tail bytes that produced cached_build_'s
    // tail child, plus the tail length (so two different-length tails
    // can't alias) plus the committed-side fence parity at that time.
    // Together they uniquely determine render_tail's output. When the
    // next build finds them all unchanged, the tail Element is
    // byte-identical to last frame's — we can skip render_tail AND
    // the children.back() assignment entirely, leaving cached_build_
    // wholly untouched.
    //
    // Why a hash instead of holding the bytes: the prior
    // std::string-of-bytes form copied the whole tail on every change
    // (`cached_tail_bytes_.assign(tail)`), which on a multi-KB live
    // paragraph dominated the per-frame cost — heap traffic
    // proportional to the unchanged-prefix size, paid every frame the
    // live line grew by one byte. Hash compare is O(tail) on the
    // refresh, but ZERO heap allocations and no copy; the recompute
    // overlaps the inline-flatten work that would have happened
    // anyway, and the steady state (tail unchanged) is a single
    // 64-bit compare with no allocation at all.
    mutable std::uint64_t cached_tail_hash_     = 0;
    mutable std::size_t   cached_tail_len_      = 0;
    mutable bool          cached_tail_in_fence_ = false;
    // Source version reflected in cached_build_'s tail child. When this
    // matches source_version_ on entry to build(), the tail bytes are
    // provably byte-identical to last frame (no append, no commit, no
    // clear, no set_content-replace has run since), so render_tail can
    // be skipped without computing the hash above. The hash fields stay
    // as a defensive secondary key for the rare path where the version
    // moved but commit_range happened to land the tail at its previous
    // shape — keeps the invariant intact under any future mutator
    // ordering changes.
    mutable std::uint64_t cached_tail_version_  = 0;
    // Reveal-clip endpoint (visible_end) reflected in the tail child.
    // When reveal_byte_clip_ moves without source_version_ moving (the
    // typewriter cursor advanced without new bytes), the cached tail
    // bytes DIFFER from the new visible-end slice and the cache MUST
    // refresh. Track separately from version so the version-equal fast
    // path doesn't return a stale tail.
    mutable std::size_t   cached_tail_clip_     = 0;

    // ── render_tail inline-parse cache ─────────────────────────────────
    // The plain-inline path of render_tail (the bottom branch — no open
    // fence, no ATX heading at tail start) is the hot path on a streaming
    // assistant message: it ran parse_inlines(body) + flatten_inline over
    // the WHOLE tail every frame, costing O(tail) per frame even though
    // most of the tail is unchanged from the previous frame.
    //
    // Strategy: cache the (content, runs) flatten_inline output for the
    // PREFIX UP TO THE LAST '\n' in the tail. Only the live line (after
    // the last '\n') is re-parsed each frame. Markdown emphasis / code
    // spans don't span newlines in practice, so splitting at '\n' is a
    // safe parser-state boundary — anything cached is independently
    // valid and the live line's parse never needs context from before
    // the split.
    //
    // Invalidation: render_tail compares the current stable prefix
    // (body up to last '\n') byte-by-byte against the cached prefix. A
    // mismatch (commit_range advanced, content was rewritten via
    // set_content's replace path, etc.) triggers a full re-parse and
    // refreshes the cache.
    //
    // Per-frame cost goes from O(tail) to O(live_line_length) on the
    // common case; on a 2 KB tail with a few KB of in-progress text,
    // that's the difference between ~2 KB of inline-parser work and
    // the ~80 B of work for the trailing partial line.
    //
    // The stable-prefix identity is now a (hash, length) pair instead
    // of a copy of the bytes — same cost-saving rationale as the tail
    // hash above (no heap copy of the prefix every time it grows by a
    // line). Collisions are vanishingly unlikely with FNV-1a 64 + a
    // length tiebreak; on miss we re-flatten, so worst-case is one
    // unnecessary refresh — never wrong output.
    mutable std::uint64_t           tail_inline_cache_prefix_hash_ = 0;
    mutable std::size_t             tail_inline_cache_prefix_len_  = 0;
    mutable std::string             tail_inline_cache_content_;
    mutable std::vector<StyledRun>  tail_inline_cache_runs_;
    // Source version at which the inline cache above was populated.
    // When source_version_ hasn't moved we can skip both the hash
    // computation AND the length compare — the cache is guaranteed
    // valid because the underlying bytes haven't changed. Only on a
    // version mismatch do we fall back to the (hash, length) key,
    // which still correctly handles the case where commit_range
    // shifted bytes in a way that left the stable-prefix slice
    // accidentally identical.
    mutable std::uint64_t           tail_inline_cache_version_     = 0;
    // ── Eager-block render cache ───────────────────────────────────────
    // The eager list / blockquote / table branches of render_tail parse
    // the proven-complete slice with parse_markdown_impl and rebuild the
    // block Element (md_block_to_element) EVERY frame. For a streaming
    // table or long list that accumulates rows, the per-frame cost is
    // O(rows) parse + O(rows) layout-element build, so the whole stream
    // is O(rows²) — the dominant cost for big in-flight tables/lists.
    //
    // The committed slice only changes when a new row terminates (a new
    // `\n` lands and extends last_committed_end). Between those events the
    // rendered block Element is byte-identical, so cache it keyed on
    // (source_version_, committed-slice length, committed-slice hash).
    // The live partial row below the block is cheap (one inline parse)
    // and is rebuilt each frame, so it stays outside this cache.
    //
    // Stores the rendered block kids as a shared list so the cached
    // Element can be re-emitted by shared_ptr copy without re-running
    // md_block_to_element. A version+hash miss re-parses once.
    mutable std::uint64_t           eager_cache_version_   = 0;
    mutable std::uint64_t           eager_cache_slice_hash_ = 0;
    mutable std::size_t             eager_cache_slice_len_ = 0;
    mutable std::vector<std::shared_ptr<const Element>> eager_cache_blocks_;

    // ── Live-table column-width floor ──────────────────────────────────
    // While a table is the live tail, the reveal cursor exposes its rows
    // one at a time, but the WHOLE table has usually already arrived in
    // source_ (the model emits it in one burst the typewriter then walks).
    // Re-deriving column widths from only the revealed rows makes the
    // table reflow horizontally every time a wider row/cell is exposed.
    // refresh_live_table_floor_() scans source_ over EVERY arrived,
    // fully-terminated row of the live table and records each column's
    // final width here; the eager/canonical table renders fold it in as a
    // floor (md::Table::min_col_widths) so the visible table sits at its
    // final widths from the first revealed row. Cached on
    // (source_version_, committed_) — recomputed only when bytes arrive or
    // the commit base moves, not every frame. Empty when the live tail is
    // not a table.
    mutable std::vector<int>        live_table_floor_;
    mutable std::uint64_t           live_table_floor_version_ =
        static_cast<std::uint64_t>(-1);
    mutable std::size_t             live_table_floor_base_ =
        static_cast<std::size_t>(-1);
    // Absolute source offset just past the last terminated table row already
    // folded into live_table_floor_. The floor is monotone (column widths
    // only ever grow as rows arrive) and each row is parsed at most once:
    // on a refresh where the table base is unchanged we parse ONLY the rows
    // in [live_table_floor_rows_end_, new_end) and max them into the floor,
    // turning the per-token cost from O(all rows) = O(rows^2)/stream into
    // O(newly-terminated rows) amortised O(1). Reset to -1 (force full
    // recompute) when the table base moves or on clear().
    mutable std::size_t             live_table_floor_rows_end_ =
        static_cast<std::size_t>(-1);

    // ── Canonical render_tail memo ──────────────────────────
    // render_tail runs a FULL parse_markdown_impl over the whole
    // terminated tail (and a second parse for the live-line merge test)
    // on every build(). build() fires every animation frame while
    // streaming, so a long block that hasn't hit a commit boundary yet
    // (e.g. a code fence with no internal blank line) gets re-parsed +
    // re-highlighted from scratch each frame — O(tail) per frame, O(tail²)
    // over the block's stream = the visible "stuck". The canonical render
    // is a pure function of (tail bytes, in_code_fence_, source_version_),
    // so memoize it on (version, len, hash, fence). On a hit we return the
    // cached Element by shared_ptr copy; on a miss we parse once.
    mutable std::uint64_t           tail_canon_cache_version_  = 0;
    mutable std::uint64_t           tail_canon_cache_hash_     = 0;
    mutable std::size_t             tail_canon_cache_len_      = 0;
    mutable bool                    tail_canon_cache_in_fence_ = false;
    mutable std::shared_ptr<const Element> tail_canon_cache_el_;

    // ── Settled shared handoff ─────────────────────────────────────────
    // Once the widget has settled (finish() ran), build() returns a stable
    // reference into cached_build_ that never changes. A host that wants to
    // STASH that tree (frozen scrollback: keep the settled turn alive on the
    // heap so the renderer can blit it by cached shared_ptr identity and
    // skip relayout) would otherwise deep-copy the whole N-child Element.
    // settled_element() materialises cached_build_ into a shared_ptr<const
    // Element> ONCE and hands back the same shared_ptr on every later call,
    // so the host copies a 16-byte pointer and the renderer keys its
    // cross-frame cache on the stable control block. Populated lazily on
    // first call after settle; reset by clear()/set_content growth.
    mutable std::shared_ptr<const Element> settled_el_;

    // ── Terminated-prefix block-render memo ─────────────────────────
    // Inside render_tail's canonical path, the terminated prefix (the
    // tail up to its last `\n`) is fully parsed AND rendered to block
    // Elements every frame the whole-tail memo above misses — i.e. on
    // EVERY frame a fast byte stream extends the live last line, because
    // appending to the live line moves tail_canon_cache_hash_ while the
    // terminated prefix is byte-identical. That whole-tail miss still
    // costs an O(terminated) parse + md_block_to_element rebuild per
    // frame = O(tail²) over a long uncommitted block (a code fence with
    // no internal blank line, the worst case): the residual "stuck" /
    // "animation freezes past N bytes" the whole-tail memo can't catch.
    //
    // The terminated prefix only changes when a new `\n` lands. Memoize
    // its rendered block Elements keyed on (version-check → len+hash,
    // in_code_fence_). On a hit we copy the cached shared_ptr Elements
    // (O(blocks) shared_ptr bumps); on a miss we parse + render once per
    // committed row, not once per frame. Stored as shared Elements so the
    // copy is cheap regardless of block size.
    mutable std::uint64_t           tail_term_cache_version_  = 0;
    mutable std::uint64_t           tail_term_cache_hash_     = 0;
    mutable std::size_t             tail_term_cache_len_      = 0;
    mutable bool                    tail_term_cache_in_fence_ = false;
    mutable std::vector<std::shared_ptr<const Element>> tail_term_cache_blocks_;
    // Absolute source byte offset where the cached terminated slice ENDS
    // (committed_ + leading-nl-skip + terminated.size()). Absolute offsets
    // are stable across the commit-behind-cursor shifts that move
    // committed_ forward every frame, so keying the term memo on this (not
    // on source_version_, which bumps every frame) lets an unchanged
    // terminated slice hit even as committed_ slides — the fix that keeps
    // render_tail from re-parsing the last block on every drain frame.
    mutable std::size_t             tail_term_cache_abs_end_  = static_cast<std::size_t>(-1);

    // ── Incremental terminated-prefix render (stable-block memo) ────────
    // The whole-tail memo above re-parses the ENTIRE terminated prefix
    // whenever a new row lands — O(terminated) per committed row. When
    // the reveal cursor lags far behind the live edge (a big burst
    // delta), that terminated prefix can be tens of KB of fully-complete
    // blocks, so the parse+render is the dominant per-frame cost of a
    // long streaming turn (stream_md_lag_test's growth-window ratio).
    //
    // But every terminated block EXCEPT the last is closed by a
    // blank-line / fence boundary that no later byte can reach back
    // across — it is byte-frozen the moment the next block opens. So we
    // only ever need to re-parse the FINAL (still-growing) terminated
    // block. This memo caches the rendered Elements for the stable
    // prefix [0, stable_len) and the byte length that produced them;
    // when stable_len advances (a block closed) we parse only the newly
    // frozen span and append. Keyed on (version, in_fence, stable_len)
    // — stable_len is monotonic within a stream, reset on clear() /
    // divergent set_content via the version check. Turns the per-row
    // terminated render from O(terminated) to O(last block).
    mutable std::uint64_t           tail_stable_cache_version_  = 0;
    mutable std::size_t             tail_stable_cache_len_      = 0;
    mutable bool                    tail_stable_cache_in_fence_ = false;
    mutable std::vector<std::shared_ptr<const Element>> tail_stable_cache_blocks_;
    // Absolute source offset where the cached stable (frozen) prefix ENDS
    // = absolute start of the still-growing last block. Stable across
    // commit shifts; the stable memo hits whenever the last block hasn't
    // advanced past a new boundary.
    mutable std::size_t             tail_stable_cache_abs_end_  = static_cast<std::size_t>(-1);

    // ── Wrapped terminated-block Elements (per-frame reshape memo) ──────
    // Even with the incremental parse memo above, render_tail rebuilt the
    // vstack of hash-keyed ComponentElement wrappers over ALL terminated
    // blocks every frame — O(terminated blocks) ComponentElement + CacheId
    // constructions per frame. On a long uncommitted tail (reveal cursor
    // lagging tens of KB) that's hundreds of blocks re-wrapped each
    // animation frame, independent of the parse. But the wrappers are a
    // pure function of the terminated bytes (their hash_id is keyed on the
    // terminated slice hash + block index), so they only change when a new
    // row terminates. Cache the wrapped vector keyed on (version,
    // terminated len, in_fence): a frame that only grows the LIVE line
    // (terminated unchanged) reuses the wrapped terminated blocks by value
    // copy of the small Element handles and only rebuilds the live-line
    // tail — O(live line), not O(terminated blocks).
    mutable std::uint64_t           tail_wrap_cache_version_  = 0;
    mutable std::size_t             tail_wrap_cache_len_      = 0;
    mutable bool                    tail_wrap_cache_in_fence_ = false;
    mutable std::size_t             tail_wrap_cache_block_count_ = 0;
    mutable std::vector<Element>    tail_wrap_cache_blocks_;
    // Absolute source offset the cached wrapped-block vector's terminated
    // slice ends at — same absolute-offset gate as the term memo.
    mutable std::size_t             tail_wrap_cache_abs_end_  = static_cast<std::size_t>(-1);

    // ── Merged last-block render memo (live-line continuation) ────────
    // render_tail's continuation test parses [last_block, end)+live_line
    // and, when it folds to one block, re-renders that merged block via
    // md_block_to_element EVERY frame — a full parse + block render on top
    // of the terminated_rendered parse of the same last block. On a burst
    // drain that's the dominant repeated cost. Memoize the merged render
    // keyed on the last block's absolute start, the terminated slice's
    // absolute end, and a hash of the trimmed live line: unchanged inputs
    // (a frame where neither the last block nor the live line moved) reuse
    // the cached Element + continuation verdict with zero parsing.
    mutable std::size_t tail_merge_cache_last_blk_abs_ = static_cast<std::size_t>(-1);
    mutable std::size_t tail_merge_cache_term_abs_end_ = static_cast<std::size_t>(-1);
    mutable std::uint64_t tail_merge_cache_live_hash_  = 0;
    mutable bool         tail_merge_cache_in_fence_    = false;
    mutable bool         tail_merge_cache_continues_   = false;
    mutable std::shared_ptr<const Element> tail_merge_cache_el_;

    // ── Per-block fold state ───────────────────────────────────────────
    // Keyed by BlockMeta::source_offset so fold state survives the rare
    // "set_content with diverging prefix" path (where commit_range is
    // replayed from scratch but the byte offsets of any unchanged
    // leading prefix are stable). Map iterators stay valid across
    // mutations of the prefix vector and ordered iteration enables
    // simple host-side "unfold all" pickers.
    //
    // Folding is purely a render-time concern: a folded block's
    // shared_ptr<const Element> stays untouched in prefix_->blocks (so
    // unfolding never has to re-parse); build() substitutes a one-line
    // stub Element. Mutating folds_ bumps fold_generation_ which is
    // mixed into the prefix ComponentElement's hash_id — this forces
    // the renderer's component_cache to miss cleanly so the stub
    // actually appears instead of replaying the cached-unfolded cells.
    std::map<std::size_t, bool>   folds_;
    std::uint64_t                 fold_generation_ = 0;

    // ── Async parse (offload) ──────────────────────────────────────────
    // For one-shot large content swaps — loading a settled message of
    // tens of KB, scrollback replay, paste of a long markdown blob.
    // The steady-state streaming path stays synchronous because each
    // delta is small and the incremental commit_range/find_block_boundary
    // chain is already O(delta).
    //
    // Model:
    //   • set_content_async() stores the requested source and, if no
    //     parse is in flight, spawns a worker that parses the whole
    //     string into a CommittedPrefix snapshot off-thread.
    //   • build() (foreground only) calls maybe_apply_async_() which
    //     checks the result slot's ready flag without blocking and,
    //     on ready, atomic-swaps the new prefix in. If the caller has
    //     since asked for a different source (latest_request_source_
    //     != source after swap), a follow-up worker is spawned —
    //     this is the single-flight coalescing Zed uses.
    //   • Worker thread runs the full markdown parse + assemble, never
    //     touches `this` except through the lock-protected slot.
    //
    // Concurrency: the slot is the only mutable state shared with the
    // worker. We use a std::mutex on the slot itself plus an atomic
    // `ready` flag so the foreground's poll is wait-free in the
    // common (not-ready) case.
    struct AsyncResult {
        // Inputs the worker consumed; foreground compares these
        // against the current request to decide whether the result
        // is still relevant.
        std::string                                 source;
        // Outputs: drop-in replacements for prefix_'s contents and
        // for the fence/committed_/ref_defs state the foreground
        // would have arrived at via the synchronous incremental
        // path.
        std::vector<std::shared_ptr<const Element>> blocks;
        std::vector<BlockMeta>                      metas;
        std::unordered_map<std::string, md::LinkRef> ref_defs;
        bool                                        in_code_fence = false;
        // Set true by the worker just before exit. Read by
        // foreground's poll. Atomic so the bool flip publishes the
        // string/vector writes above (the mutex provides full
        // sync; the atomic lets the foreground skip the lock when
        // the worker hasn't finished).
        std::atomic<bool>                           ready{false};
    };
    mutable std::shared_ptr<AsyncResult>     async_slot_;
    // Wrapper around the slot mutex so StreamingMarkdown stays
    // movable. A raw std::mutex is neither copyable nor movable, which
    // would delete the implicit move ops of the whole class — but the
    // class IS moved (per-turn reset `m.md = StreamingMarkdown{}`, the
    // Model move in update()). Moving a StreamingMarkdown is only ever
    // done on a quiescent instance (no worker mid-parse), so the lock
    // carries no live state worth transferring: move/copy simply leave
    // a freshly-constructed mutex in place. The other async members
    // (slot shared_ptr, latest_source optional) move normally and stay
    // consistent.
    struct MovableMutex {
        std::mutex m;
        MovableMutex() = default;
        MovableMutex(const MovableMutex&) noexcept {}
        MovableMutex(MovableMutex&&) noexcept {}
        MovableMutex& operator=(const MovableMutex&) noexcept { return *this; }
        MovableMutex& operator=(MovableMutex&&) noexcept { return *this; }
    };
    mutable MovableMutex                     async_mu_holder_;
    std::mutex& async_mu_() const noexcept { return async_mu_holder_.m; }
    // The most recent source set_content_async was called with.
    // Compared against async_slot_->source when a result lands to
    // decide whether to apply it directly (current) or queue another
    // parse (stale). Empty optional == no async pending.
    mutable std::optional<std::string>       async_latest_source_;

    // Apply any ready async result. Foreground-only. No-op if no
    // result is ready. May spawn a follow-up worker.
    void maybe_apply_async_() const;
    // Spawn a worker on the requested source. Foreground-only; safe
    // to call repeatedly — coalesces by storing the request in
    // async_latest_source_ and only spawning when no in-flight worker
    // exists.
    void spawn_async_worker_(std::string source) const;

    // Find the end of the last complete block boundary.
    // Returns the byte offset up to which blocks are "complete".
    // Non-const because it advances the resumable scanner state above.
    [[nodiscard]] size_t find_block_boundary() noexcept;

    // Reveal-paced progressive commit: commit every pending boundary the
    // reveal cursor has swept past (largest ledger entry ≤ cursor byte).
    // The visible cells are unchanged by construction — render_tail's
    // canonical path already renders terminated bytes in their committed
    // shape, and the cursor has already revealed them — so this is pure
    // internal bookkeeping that keeps the live tail bounded to ~one
    // block + the cursor's lag window. No-op unless reveal_fx_ && live_.
    // Called from append_safe (on new bytes) and from build() via
    // const_cast (cursor advances between deltas / during a wire pause).
    void commit_revealed_();

    // Parse [committed_, boundary) — stash its ref defs, render its blocks.
    void commit_range(size_t boundary);

    // Render the uncommitted tail as a monotonic in-progress paragraph
    // (or as plain text inside an open code fence).  See class header.
    // render_tail is the monotonicity funnel: it compares render_tail_inner's
    // eager/inline shape against the canonical committed-parse render and
    // emits whichever keeps the height from dropping at the commit seam.
    [[nodiscard]] Element render_tail(std::string_view tail) const;

    // The eager/inline tail builder — picks the in-progress shape
    // (open fence, ATX heading, eager list/table/quote, inline
    // fallback). render_tail wraps it. See its body.
    [[nodiscard]] Element render_tail_inner(std::string_view tail) const;

    // Render an eager-block slice (proven-complete list/quote/table rows)
    // via parse_markdown_impl + md_block_to_element, memoizing the
    // resulting block Elements keyed on the slice bytes so an unchanged
    // slice re-emits by shared_ptr copy instead of re-parsing. Appends
    // the rendered blocks to `kids`.
    void render_eager_slice(std::string_view slice,
                            std::vector<Element>& kids) const;

    // Refresh live_table_floor_ for THIS frame. When the uncommitted tail
    // begins (after blank lines) with a GFM table, scans source_ over all
    // already-arrived fully-terminated rows of that table and records each
    // column's width; otherwise clears the floor. Cached on
    // (source_version_, committed_). Cheap + idempotent. Defined in
    // render_tail.cpp.
    void refresh_live_table_floor_() const;

    // If `block` is a md::Table and a live floor is set, stamp the floor
    // onto it (min_col_widths) so md_block_to_element reserves the final
    // column widths. No-op for non-tables / empty floor / a table that
    // already carries a floor. Defined in render_tail.cpp.
    void apply_live_table_floor_(md::Block& block) const;

    // Live-mode finalize: returns cached_build_ when not live or reveal_fx
    // is off; otherwise builds cached_live_ (the animated scramble/gradient/
    // caret overlay over cached_build_'s tail) and requests an animation
    // frame. Defined in reveal_fx.cpp. build() calls this at every return.
    [[nodiscard]] const Element& render_live_overlay_() const;

    // Advance reveal_cp_ for this frame. Called from build() BEFORE the
    // cache short-circuit so a finalize-ramp completion flips live_ off
    // on the same frame instead of the next. Returns true when the
    // build cache should be invalidated (currently only for ramp
    // completion). Defined in reveal_fx.cpp.
    bool advance_reveal_cursor_() const;

    // Internal append — assumes bytes are already codepoint-clean.  Public
    // entry points (feed / append / set_content) route through StreamSink.
    void append_safe(std::string_view safe_bytes);

public:
    StreamingMarkdown() = default;

    /// Feed bytes — the canonical streaming entry point.  Bytes that
    /// would split a multi-byte codepoint or an in-flight ANSI escape
    /// are held internally until their continuation arrives.  Safe to
    /// call with any chunk size, including 1 byte.
    void feed(std::string_view bytes) { append(bytes); }

    /// Replace the full content (for SSE-style streaming that resends
    /// the entire string each frame, like `msg.content = ...`).
    void set_content(std::string_view content);

    /// Same as set_content but offloads the work to a worker thread
    /// when the content diverges from the current prefix (the case
    /// that would otherwise stall the render thread on a large parse,
    /// e.g. loading an old thread, pasting a long blob, snapshotting
    /// scrollback). For pure-append growth (the streaming case) this
    /// stays on the synchronous incremental path — the per-delta cost
    /// is already O(delta) and the thread-handoff overhead would
    /// dominate.
    ///
    /// While the worker is in flight build() keeps returning the
    /// PREVIOUS frame's element tree (Zed's "keep existing content
    /// visible until new parse completes" trick), so the UI never
    /// blanks. Coalesces: if multiple set_content_async calls land
    /// before the worker finishes, only the most recent one is parsed
    /// when the worker drains.
    void set_content_async(std::string_view content);

    /// True iff a background parse is in flight. Hosts can use this
    /// to render a subtle "still loading" hint, but the widget itself
    /// is fully usable while pending (build() returns the previous
    /// element tree).
    [[nodiscard]] bool is_parsing() const noexcept;

    /// Append bytes incrementally.  Equivalent to feed(); kept for
    /// back-compat with existing call sites.
    void append(std::string_view text);

    /// Finalize: drain any held bytes from the StreamSink, commit the
    /// remaining tail as full markdown, and lock the widget.  Call at
    /// end-of-stream.
    void finish();

    /// Reset all state for a new stream.
    void clear();

    /// Set whether this widget is currently "live" — i.e. the
    /// host expects more bytes to arrive. While live, build() paints
    /// a blinking cursor caret after the tail so the user sees the
    /// stream is alive even mid-token. Re-renders are driven by
    /// request_animation_frame() so the blink happens at ~30 fps
    /// regardless of byte arrival rate. Default: false (settled).
    void set_live(bool live) noexcept {
        // If the host explicitly forces live_=false (cancel / clear / hard
        // reset path) we drop any pending finalize ramp too — there's
        // nothing left to glide toward.
        if (!live) finalize_deadline_ms_ = 0;
        // The Tracked<> wrapper around live_ auto-bumps build_dirty_ on
        // every assignment — the has_tail / cache-shape coupling is
        // structural now, not a manual pairing. No need to check the
        // old value either: a same-value assign is harmless (one wasted
        // rebuild at most) and not on a hot path.
        live_ = live;
    }

    /// Begin a finalize ramp: the host has decided the stream is ending
    /// (last delta seen / message_stop on the wire). The widget stays in
    /// live mode but the reveal cursor is guaranteed to reach the live
    /// edge within `ramp_ms` milliseconds, after which build() will flip
    /// live_ to false itself and return the settled cached_build_ — so
    /// settle never finds a backlog to dump in a single frame (the
    /// "sudden render" the user sees as the streaming indicator vanishes).
    ///
    /// Idempotent: calling it again while a ramp is already in flight is
    /// a no-op (the original deadline stands). Cleared automatically when
    /// the ramp completes, on clear(), or on set_live(false). If called
    /// while !live_ or while the cursor is already at the edge, completes
    /// immediately on the next build() with no visible animation.
    ///
    /// `ramp_ms` is a hard upper bound on the reveal time of any
    /// outstanding backlog; the normal drain rate still applies and a
    /// small backlog will finish sooner.
    void request_finalize(int ramp_ms) noexcept;

    /// True while a finalize ramp is in flight. Hosts use it to keep the
    /// 16 ms animation frame armed through the ramp so the cursor keeps
    /// gliding to the edge.
    [[nodiscard]] bool is_finalizing() const noexcept {
        return finalize_deadline_ms_ != 0;
    }

    /// Force the reveal cursor to the live edge for the NEXT build(),
    /// WITHOUT ending live mode. Unlike request_finalize (which ramps the
    /// cursor over time and then flips live_=false), this is an instant,
    /// single-frame snap: reveal_cp_ jumps to the total codepoint count so
    /// the trailing tail renders FULLY revealed — no ghost-blanked cells,
    /// no scramble — while the widget stays live_ and keeps animating on
    /// subsequent frames as more bytes arrive.
    ///
    /// The scrollback-safety use case: a terminal HEIGHT SHRINK autonomously
    /// pushes the top viewport rows into immutable native scrollback. If the
    /// live-edge tail (which decorate_text_reveal ghost-blanks + scrambles
    /// per-codepoint) sits among those rows, its unrevealed cells freeze in
    /// scrollback as stale blanks/garbage — the row is lost. A resize is a
    /// discrete user event, so momentarily rendering the tail fully-revealed
    /// for that one frame is imperceptible and leaves no ghosted row for the
    /// terminal to strand. The host calls this from its resize hook (when it
    /// detects the viewport height dropped) before Strata composes.
    ///
    /// Cheap + idempotent: if the cursor is already at the edge it's a
    /// no-op. Bumps build_dirty_ so the next build() re-renders the tail.
    void snap_reveal_to_edge() noexcept;

    /// Opt into the animated streaming-reveal effect (gradient trail +
    /// scramble + pulsing caret). Off by default — see `reveal_fx_`.
    /// When off, streamed text appears in its final style with no
    /// per-frame color/glyph churn (calm, flicker-free on every
    /// terminal).
    void set_reveal_fx(bool on) noexcept { reveal_fx_ = on; }

    /// Override the reveal cursor pacing. `floor_cps` is the
    /// minimum codepoints-per-second the typewriter walks at (also
    /// the steady-state when the wire is keeping up); `drain_secs`
    /// is the target window to clear a backlog after a burst. Both
    /// must be > 0. Persists across set_reveal_fx() toggles.
    void set_reveal_pacing(double floor_cps, double drain_secs) noexcept {
        if (floor_cps  > 0.0) reveal_floor_cps_  = floor_cps;
        if (drain_secs > 0.0) reveal_drain_secs_ = drain_secs;
    }
    [[nodiscard]] bool is_live() const noexcept { return live_; }

    /// True while the internal reveal cursor is still catching up to the
    /// available source (the typewriter hasn't reached the live edge).
    /// Hosts use this to keep the 16 ms animation frame armed across a
    /// wire pause so the reveal keeps gliding instead of stalling between
    /// bursts. reveal_cp_ is fractional; compare against the source
    /// codepoint count is approximated by byte size (cursor < bytes is a
    /// safe superset — worst case one extra armed frame at settle).
    [[nodiscard]] bool reveal_in_progress() const noexcept {
        return live_ && reveal_fx_
            && reveal_cp_ < static_cast<double>(source_.size());
    }

    /// Build the element tree: cached blocks + monotonic tail.  Returns
    /// a reference into the per-frame cache; valid until the next
    /// mutator call.
    [[nodiscard]] const Element& build() const;

    /// Zero-copy handoff of a SETTLED widget's final tree. Only valid once
    /// the widget has finished (is_live() == false); returns nullptr while
    /// still live. Materialises build()'s result into a shared_ptr<const
    /// Element> once and returns the same pointer on every later call, so a
    /// host stashing the settled turn (frozen scrollback) copies a 16-byte
    /// pointer instead of deep-copying the whole block tree, and the
    /// renderer can key its cross-frame cache on the stable control block.
    /// Invalidated by clear() / set_content growth (both re-live the widget).
    [[nodiscard]] std::shared_ptr<const Element> settled_element() const {
        if (live_) return nullptr;
        // Re-materialise if never built, or if the widget was mutated since
        // (build_dirty_ set by any feed/clear/set_content after a settle) so
        // the stash never hands back a stale tree.
        if (!settled_el_ || build_dirty_) {
            settled_el_ = std::make_shared<const Element>(build());
        }
        return settled_el_;
    }

    /// Current full source text (codepoint-clean; never contains a
    /// half-written multi-byte sequence).
    [[nodiscard]] const std::string& source() const noexcept { return source_; }

    // ── Block introspection (Zed-style byte-offset keyed UI state) ──

    /// Number of committed (parsed) top-level blocks. Excludes the
    /// in-progress tail.
    [[nodiscard]] std::size_t block_count() const noexcept {
        return prefix_->metas.size();
    }

    /// Read-only view of a committed block's metadata. Index in
    /// [0, block_count()). Reference is valid until the next mutator
    /// (feed/append/set_content/clear).
    [[nodiscard]] const BlockMeta& block_meta(std::size_t i) const noexcept {
        return prefix_->metas[i];
    }

    /// Binary search: which committed block contains source byte
    /// offset `off`, if any. Returns the index in [0, block_count())
    /// such that block_meta(i).source_offset <= off < source_end,
    /// or nullopt if `off` lies in the uncommitted tail / past EOS.
    /// O(log block_count()).
    [[nodiscard]] std::optional<std::size_t>
    block_for_offset(std::size_t off) const noexcept;

    /// Toggle the fold state of the block at `source_offset` (must
    /// match a BlockMeta::source_offset exactly). Returns the new
    /// folded state (true = folded). No-op + returns false for an
    /// unknown offset.
    bool toggle_fold(std::size_t source_offset);

    /// Explicit setter — useful for batch "fold all code blocks > N
    /// lines" passes. Same offset semantics as toggle_fold.
    void set_fold(std::size_t source_offset, bool folded);

    /// Query.
    [[nodiscard]] bool is_folded(std::size_t source_offset) const noexcept;

    /// Clear every fold. Cheap; mostly for tests.
    void unfold_all();

    /// Fold every committed block whose line_count exceeds
    /// `threshold_lines`, restricted to the kinds in
    /// `kinds_mask` (bitmask over BlockKind). Idempotent. Useful
    /// for the host to call once a settled assistant message is
    /// committed so very long code blocks collapse to a one-row
    /// stub by default — the user can still toggle them open.
    ///
    /// Pass `(1u << static_cast<unsigned>(BlockKind::CodeBlock))` to
    /// fold only long code blocks (the common preset).
    void auto_fold_long_blocks(std::uint16_t threshold_lines,
                               std::uint32_t kinds_mask);
};

} // namespace maya
