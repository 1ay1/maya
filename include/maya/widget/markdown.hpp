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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "../element/builder.hpp"
#include "../text/stream_sink.hpp"

namespace maya {
namespace md {

// ============================================================================
// Inline AST nodes (within paragraphs/headings)
// ============================================================================

struct Text        { std::string content; };
struct Bold        { std::vector<struct Inline> children; };
struct Italic      { std::vector<struct Inline> children; };
struct BoldItalic  { std::vector<struct Inline> children; };
struct Code        { std::string content; };
struct Link        { std::string text; std::string url; std::string title; };
struct Image       { std::string alt;  std::string url; std::string title; };
struct Strike      { std::vector<struct Inline> children; };
struct Highlight   { std::vector<struct Inline> children; };    // ==text== or <mark>
struct Sub         { std::vector<struct Inline> children; };    // ~x~ or <sub>
struct Sup         { std::vector<struct Inline> children; };    // ^x^ or <sup>
struct Kbd         { std::vector<struct Inline> children; };    // <kbd>
struct Abbr        { std::string title; std::vector<struct Inline> children; }; // <abbr title="…">
struct Mention     {
    enum class Kind : uint8_t { User, Issue, CrossRepo };
    Kind        kind;
    std::string display;   // "@alice" / "#42" / "foo/bar#7"
    std::string url;       // best-effort GitHub URL
};
struct FootnoteRef { std::string label; };
struct HardBreak   {};

struct Inline {
    using Variant = std::variant<Text, Bold, Italic, BoldItalic, Code,
                                 Link, Image, Strike, Highlight, Sub, Sup,
                                 Kbd, Abbr, Mention, FootnoteRef, HardBreak>;
    Variant inner;

    Inline(Text t)        : inner(std::move(t)) {}
    Inline(Bold b)        : inner(std::move(b)) {}
    Inline(Italic i)      : inner(std::move(i)) {}
    Inline(BoldItalic bi) : inner(std::move(bi)) {}
    Inline(Code c)        : inner(std::move(c)) {}
    Inline(Link l)        : inner(std::move(l)) {}
    Inline(Image im)      : inner(std::move(im)) {}
    Inline(Strike s)      : inner(std::move(s)) {}
    Inline(Highlight h)   : inner(std::move(h)) {}
    Inline(Sub sb)        : inner(std::move(sb)) {}
    Inline(Sup sp)        : inner(std::move(sp)) {}
    Inline(Kbd k)         : inner(std::move(k)) {}
    Inline(Abbr a)        : inner(std::move(a)) {}
    Inline(Mention m)     : inner(std::move(m)) {}
    Inline(FootnoteRef f) : inner(std::move(f)) {}
    Inline(HardBreak)     : inner(HardBreak{}) {}
};

// ============================================================================
// Block AST nodes
// ============================================================================

struct Paragraph   { std::vector<Inline> spans; };
struct Heading     { int level; std::vector<Inline> spans; };
struct CodeBlock   { std::string content; std::string lang; };
struct Blockquote  { std::vector<struct Block> children; };

struct ListItem {
    std::vector<Inline> spans;                // first-line inline content
    std::vector<struct Block> children;       // sub-blocks (nested lists, multi-para)
    std::optional<bool> checked;              // nullopt = normal, true/false = task
};
struct List        { std::vector<ListItem> items; bool ordered; int start_num = 1; };

struct HRule       {};
struct TableCell   { std::vector<Inline> spans; };
struct TableRow    { std::vector<TableCell> cells; };
// Per-column alignment from the GFM `|:--|:-:|--:|` delimiter row.
// `aligns.size()` is always equal to `header.cells.size()`. Default
// (no colons on either side) is `Left`.
enum class TableAlign : std::uint8_t { Left, Center, Right };
struct Table       {
    TableRow header;
    std::vector<TableRow> rows;
    std::vector<TableAlign> aligns;
};
struct FootnoteDef { std::string label; std::vector<struct Block> children; };

// GitHub alert block: `> [!NOTE] ...`
struct Alert {
    enum class Kind : uint8_t { Note, Tip, Important, Warning, Caution };
    Kind kind = Kind::Note;
    std::vector<struct Block> children;
};

// Definition list: term  \n  : def \n  : another def
struct DefItem { std::vector<Inline> term; std::vector<std::vector<struct Block>> defs; };
struct DefList { std::vector<DefItem> items; };

// Collapsible <details>/<summary> section (rendered as a titled blockquote)
struct Details {
    std::vector<Inline>       summary;
    std::vector<struct Block> body;
};

// Arbitrary raw-HTML block — rendered as plain monospaced text.
struct HtmlBlock { std::string content; };

struct Block {
    using Variant = std::variant<Paragraph, Heading, CodeBlock,
                                 Blockquote, List, HRule, Table,
                                 FootnoteDef, Alert, DefList, Details, HtmlBlock>;
    Variant inner;

    Block(Paragraph p)   : inner(std::move(p)) {}
    Block(Heading h)     : inner(std::move(h)) {}
    Block(CodeBlock c)   : inner(std::move(c)) {}
    Block(Blockquote b)  : inner(std::move(b)) {}
    Block(List l)        : inner(std::move(l)) {}
    Block(HRule)         : inner(HRule{}) {}
    Block(Table t)       : inner(std::move(t)) {}
    Block(FootnoteDef f) : inner(std::move(f)) {}
    Block(Alert a)       : inner(std::move(a)) {}
    Block(DefList d)     : inner(std::move(d)) {}
    Block(Details d)     : inner(std::move(d)) {}
    Block(HtmlBlock h)   : inner(std::move(h)) {}
};

// Reference-link definition: `[label]: url "title"`.  Keys are lowercased.
struct LinkRef { std::string url; std::string title; };

struct Document {
    std::vector<Block>                              blocks;
    std::unordered_map<std::string, LinkRef>        ref_defs;
};

} // namespace md

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
    struct CommittedPrefix {
        std::vector<std::shared_ptr<const Element>> blocks;
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
    mutable bool    build_dirty_  = true;
    mutable size_t  cached_tail_size_ = 0;   // tail length when cache was built
    // Generation reflected in cached_build_'s prefix component slot —
    // used to short-circuit the rebuild when only the tail changed.
    mutable std::uint64_t cached_prefix_gen_ = 0;
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

    // Find the end of the last complete block boundary.
    // Returns the byte offset up to which blocks are "complete".
    // Non-const because it advances the resumable scanner state above.
    [[nodiscard]] size_t find_block_boundary() noexcept;

    // Parse [committed_, boundary) — stash its ref defs, render its blocks.
    void commit_range(size_t boundary);

    // Render the uncommitted tail as a monotonic in-progress paragraph
    // (or as plain text inside an open code fence).  See class header.
    [[nodiscard]] Element render_tail(std::string_view tail) const;

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

    /// Append bytes incrementally.  Equivalent to feed(); kept for
    /// back-compat with existing call sites.
    void append(std::string_view text);

    /// Finalize: drain any held bytes from the StreamSink, commit the
    /// remaining tail as full markdown, and lock the widget.  Call at
    /// end-of-stream.
    void finish();

    /// Reset all state for a new stream.
    void clear();

    /// Build the element tree: cached blocks + monotonic tail.  Returns
    /// a reference into the per-frame cache; valid until the next
    /// mutator call.
    [[nodiscard]] const Element& build() const;

    /// Current full source text (codepoint-clean; never contains a
    /// half-written multi-byte sequence).
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
};

} // namespace maya
