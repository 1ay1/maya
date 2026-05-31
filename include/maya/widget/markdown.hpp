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
    bool live_ = false;

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
    // frame so the cursor blinks. Cheap to rebuild (two Element
    // copies into a vstack) so we don’t cache it across frames.
    mutable Element cached_live_;
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
    mutable std::mutex                       async_mu_;
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

    // Parse [committed_, boundary) — stash its ref defs, render its blocks.
    void commit_range(size_t boundary);

    // Render the uncommitted tail as a monotonic in-progress paragraph
    // (or as plain text inside an open code fence).  See class header.
    [[nodiscard]] Element render_tail(std::string_view tail) const;

    // Live-mode finalize: returns cached_build_ when not live or reveal_fx
    // is off; otherwise builds cached_live_ (the animated scramble/gradient/
    // caret overlay over cached_build_'s tail) and requests an animation
    // frame. Defined in reveal_fx.cpp. build() calls this at every return.
    [[nodiscard]] const Element& render_live_overlay_() const;

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
    void set_live(bool live) noexcept { live_ = live; }

    /// Opt into the animated streaming-reveal effect (gradient trail +
    /// scramble + pulsing caret). Off by default — see `reveal_fx_`.
    /// When off, streamed text appears in its final style with no
    /// per-frame color/glyph churn (calm, flicker-free on every
    /// terminal).
    void set_reveal_fx(bool on) noexcept { reveal_fx_ = on; }
    [[nodiscard]] bool is_live() const noexcept { return live_; }

    /// Build the element tree: cached blocks + monotonic tail.  Returns
    /// a reference into the per-frame cache; valid until the next
    /// mutator call.
    [[nodiscard]] const Element& build() const;

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
