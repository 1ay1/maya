// build.cpp — StreamingMarkdown::build(), the per-frame element assembler.
//
// Four tiers, fastest-first: untouched → cached; tail-only fast path
// (mutate children.back() in place); commit fast path (swap the prefix
// ComponentElement for one with a fresh generation hash_id); full rebuild.
// A monotonic source_version_ lets each tier short-circuit with O(1)
// compares. The committed prefix is wrapped in one generation-keyed
// ComponentElement so the renderer's component cache amortises its paint.
// The live-mode overlay is delegated to render_live_overlay_ (reveal_fx.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/render/cache_id.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

#include "maya/widget/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::streaming::fnv1a64;

const Element& StreamingMarkdown::build() const {

    // Poll any landed async parse — may mutate prefix_/source_ and
    // flip build_dirty_, so it must run BEFORE the early-out.
    maybe_apply_async_();

    // Advance the reveal cursor and republish the tail clip BEFORE
    // the cache short-circuit. Three things this fixes:
    //   1. The clip is current for THIS frame, not the next — no
    //      one-frame flash where a newly-arrived eager-table burst
    //      lands unrevealed then collapses.
    //   2. If reveal_cp_ advances without source changing, we bump
    //      build_dirty_ here so the cached path falls through to
    //      rebuild with the new clip — keeps the typewriter running
    //      through wire stalls.
    //   3. Finalize-ramp completion (which flips live_ off) is
    //      observed before the cache check, so the next frame doesn't
    //      serve a stale live-mode build.
    if (advance_reveal_cursor_()) {
        build_dirty_ = true;
    }

    // Untouched-since-last-build: return cached. Dominant case when the
    // widget is idle (no streaming).
    if (!build_dirty_) return render_live_overlay_();

    // Visible end-of-source for THIS frame. When reveal_fx_ && live_
    // and advance_reveal_cursor_ has run at least once, clamp to the
    // cursor's byte clip so render_tail can't eager-style bytes the
    // typewriter hasn't reached yet (the heading underline / code-fence
    // chrome / table rules burst). Otherwise it's source_.size().
    const std::size_t visible_end =
        (reveal_fx_ && live_
         && reveal_byte_clip_ != static_cast<std::size_t>(-1))
            ? std::min(source_.size(), reveal_byte_clip_)
            : source_.size();
    std::string_view tail = (committed_ < visible_end)
        ? std::string_view{source_}.substr(committed_, visible_end - committed_)
        : std::string_view{};
    // Keep the tail slot PRESENT while streaming with a committed
    // prefix, even when the tail is momentarily empty. Between two
    // appends that both end on `\n\n`, commit_range may consume all of
    // source_ — for that one frame `tail` is empty. If has_tail
    // tracked emptiness directly, the outer vstack would collapse from
    // [prefix, gap, tail] to [prefix] and DROP the trailing .gap(1) row,
    // then re-add it the instant the next byte arrives: a 1-row down/up
    // bounce at EVERY block boundary that ripples the composer/status
    // bar below (the "chrome flickers while md renders" symptom). While
    // live, reserve the tail slot so the inter-block gap stays put; the
    // empty tail renders as a 0-row element, and the gap matches the
    // steady state the next arriving byte lands in. finish()/settle
    // clears live_, so a genuinely-finished message with no tail does
    // NOT carry a dangling gap.
    const bool has_prefix = !prefix_->blocks.empty();
    const bool has_tail   = !tail.empty() || (live_ && has_prefix);

    // ── Empty special case ──
    if (!has_prefix && !has_tail) {
        cached_build_      = Element{TextElement{""}};
        cached_tail_size_  = 0;
        cached_prefix_gen_ = prefix_->generation;
        cached_has_tail_   = false;
        cached_has_prefix_ = false;
        cached_tail_version_     = source_version_;
        cached_tail_clip_        = visible_end;
        build_dirty_       = false;
        return render_live_overlay_();
    }

    // ── Tail-only fast path ──
    //
    // The hot streaming case: prefix unchanged, has_tail/has_prefix
    // shape unchanged, only the tail's content moved. We can mutate
    // cached_build_'s tail child (the LAST child of the outer vstack)
    // in place — every prefix child keeps its address and per-component
    // caches stay warm, while only render_tail's lightweight inline-
    // cache path runs.
    //
    // Sub-fast-path: if the tail body bytes AND the committed-side
    // fence parity are byte-identical to the cache, render_tail's
    // output is provably identical to what's already in
    // children.back() — skip the call and the assignment entirely,
    // leaving cached_build_ wholly untouched. The bytes-equality check
    // matters for the case where build_dirty_ flipped (e.g., a feed
    // arrived) but the resulting tail body didn't actually change
    // (commit_range absorbed the new bytes; the body shape happened
    // to round-trip; any future code path that bumps build_dirty_
    // without changing the tail).

    // Helper: build the prefix ComponentElement for the current
    // generation. Used by both the full-rebuild path and the
    // commit-fast-path below. Hoisted into a lambda so the hash_id
    // construction and lambda-capture wiring don't drift between the
    // two call sites (a previous bug where they did caused the prefix
    // ComponentElement to skip its cache invalidation on commit and
    // ghost the previous frame's prefix bitmap into the new prefix's
    // layout slot).
    auto make_prefix_component = [&]() -> Element {
        auto p = prefix_;
        // Snapshot the fold map so the render lambda's behaviour is
        // stable per ComponentElement instance — unrelated edits
        // (typing into the composer) that don't change the prefix
        // won't reshape this component, but a fold toggle bumps
        // fold_generation_ which changes hash_id and forces a
        // re-render with the updated map.
        std::map<std::size_t, bool> fold_snapshot = folds_;
        ComponentElement comp;
        comp.hash_id = CacheIdBuilder{}
            .add(std::string_view{"strmd-prefix"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(static_cast<std::uint64_t>(p->generation))
            .add(static_cast<std::uint64_t>(fold_generation_))
            .build();
        comp.render = [p, inst = instance_id_,
                       folds = std::move(fold_snapshot)](int /*w*/, int /*h*/) -> Element {
            std::vector<Element> kids;
            kids.reserve(p->blocks.size());
            for (std::size_t i = 0; i < p->blocks.size(); ++i) {
                const auto& meta = (i < p->metas.size())
                    ? p->metas[i] : BlockMeta{};
                auto fit = folds.find(meta.source_offset);
                const bool folded = fit != folds.end() && fit->second;

                // Each committed block is wrapped in its OWN hash-keyed
                // ComponentElement. The key is stable CONTENT identity
                // (instance + source byte range + fold state), never a
                // pointer — committed blocks are immutable, so a block's
                // key is fixed the moment it commits. The renderer's
                // hash-keyed cell cache therefore paints each block once
                // and blits it (memcpy/row) on every later frame. When a
                // commit appends block N+1, blocks 0..N keep their keys
                // and blit; only the new block renders. Per-commit cost
                // is O(new block), not O(transcript). A fold toggle
                // flips `folded`, changing that one block's key so it
                // re-renders into its stub on the next frame.
                //
                // Why this is safe where the earlier per-block attempt
                // wasn't: that one used the Element{shared_ptr} ctor
                // keyed on pointer identity (id_for_shared), which hit
                // the pointer-keyed cache's stale-height path and left
                // blank rows when content overflowed into native
                // scrollback. Hash-keyed entries go through the
                // content-stable cell cache whose capture path already
                // guards OOB reads and width changes.
                ComponentElement block_comp;
                block_comp.hash_id = CacheIdBuilder{}
                    .add(std::string_view{"strmd-block"})
                    .add(static_cast<std::uint64_t>(inst))
                    .add(static_cast<std::uint64_t>(meta.source_offset))
                    .add(static_cast<std::uint64_t>(meta.source_end))
                    .add(static_cast<std::uint64_t>(folded ? 1u : 0u))
                    .build();

                if (folded) {
                    // Render a single-line stub. Use a styled
                    // dim summary derived from BlockMeta so a
                    // long code block collapses to one row
                    // labelled with its language and line
                    // count.
                    const char* kind_label = "block";
                    switch (meta.kind) {
                        case BlockKind::CodeBlock:  kind_label = "code";       break;
                        case BlockKind::Table:      kind_label = "table";      break;
                        case BlockKind::List:       kind_label = "list";       break;
                        case BlockKind::Blockquote: kind_label = "blockquote"; break;
                        case BlockKind::Paragraph:  kind_label = "paragraph";  break;
                        case BlockKind::Heading:    kind_label = "heading";    break;
                        case BlockKind::HtmlBlock:  kind_label = "html";       break;
                        default: break;
                    }
                    char buf[160];
                    if (!meta.lang.empty()) {
                        std::snprintf(buf, sizeof(buf),
                            "\xe2\x96\xb8 %u line%s of %s (%s) hidden \xe2\x80\x94 unfold",
                            static_cast<unsigned>(meta.line_count),
                            meta.line_count == 1 ? "" : "s",
                            kind_label, meta.lang.c_str());
                    } else {
                        std::snprintf(buf, sizeof(buf),
                            "\xe2\x96\xb8 %u line%s of %s hidden \xe2\x80\x94 unfold",
                            static_cast<unsigned>(meta.line_count),
                            meta.line_count == 1 ? "" : "s",
                            kind_label);
                    }
                    std::string stub{buf};
                    block_comp.render =
                        [stub = std::move(stub)](int, int) -> Element {
                            return Element{TextElement{
                                .content = stub,
                                .style   = Style{}.with_fg(colors::strike_fg).with_dim(),
                            }};
                        };
                } else {
                    auto blk = p->blocks[i];   // shared_ptr<const Element>
                    block_comp.render = [blk](int, int) -> Element {
                        return *blk;
                    };
                }
                kids.push_back(Element{std::move(block_comp)});
            }
            return detail::vstack().gap(1)(std::move(kids)).build();
        };
        return Element{std::move(comp)};
    };

    if (cached_prefix_gen_ == prefix_->generation
        && cached_fold_gen_  == fold_generation_
        && cached_has_prefix_ == has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t prefix_n =
            has_prefix ? prefix_->blocks.size() : 0u;
        const std::size_t expected = prefix_n + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            if (has_tail) {
                // Fast path: source_version_ matches what produced the
                // current cached tail AND the reveal clip is unchanged —
                // tail bytes provably byte-identical to last frame. The
                // clip endpoint is part of the tail's identity because
                // build() truncates `tail` by it before render_tail sees
                // it; if the clip moved with version unchanged (typewriter
                // cursor advanced without new bytes), the tail bytes
                // DIFFER from what's in cached_build_, and skipping the
                // re-render would leave the stale longer tail visible —
                // the reveal effectively wouldn't run.
                //
                // Slow path: version moved OR clip moved, fall back to
                // the (fence, length, FNV-1a) digest. This catches the
                // rare case where commit_range fired but the tail
                // happened to land at the same bytes — lets us still
                // skip render_tail when the body round-tripped.
                bool tail_unchanged;
                std::uint64_t tail_hash = 0;
                if (cached_tail_version_ == source_version_
                    && cached_tail_clip_ == visible_end)
                {
                    tail_unchanged = true;
                } else {
                    tail_hash = fnv1a64(tail);
                    tail_unchanged =
                        cached_tail_in_fence_ == in_code_fence_
                        && cached_tail_len_   == tail.size()
                        && cached_tail_hash_  == tail_hash;
                }
                if (!tail_unchanged) {
                    // Swap in the new tail. children.back() is the
                    // tail slot regardless of whether prefix is
                    // present:
                    //   [prefix, tail] → children[1]
                    //   [tail]         → children[0]
                    box.children.back() = render_tail(tail);
                    cached_tail_hash_     = tail_hash;
                    cached_tail_len_      = tail.size();
                    cached_tail_in_fence_ = in_code_fence_;
                }
                cached_tail_version_     = source_version_;
                cached_tail_clip_        = visible_end;
            }
            cached_tail_size_ = tail.size();
            build_dirty_      = false;
            return render_live_overlay_();
        }
    }

    // ── Commit fast path ──
    //
    // Generation moved (commit_range fired), but the outer vstack's
    // shape (has_prefix / has_tail) is unchanged. We can mutate the
    // existing BoxElement in place: swap children[0] (the prefix
    // ComponentElement) for a fresh one carrying the new generation in
    // its hash_id, and refresh the tail child if needed. The outer
    // BoxElement's layout/border/padding config stays as-is — no
    // builder pipeline, no ElementBuilder allocation, no .build()
    // call, no walk over the unrelated metadata fields.
    //
    // Without this branch every commit (which happens at every
    // paragraph boundary during a long streaming reply, ~10-100x per
    // turn) fell through to the full-rebuild path, which recreates
    // the outer vstack from scratch — a small cost individually but
    // taken at exactly the rate the user is most sensitive to
    // (visible UI tearing / pause when each new block lands).
    if ((cached_prefix_gen_ != prefix_->generation
         || cached_fold_gen_  != fold_generation_)
        && cached_has_prefix_ == has_prefix && has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t expected = 1u + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            box.children[0] = make_prefix_component();
            if (has_tail) {
                bool tail_unchanged;
                std::uint64_t tail_hash = 0;
                if (cached_tail_version_ == source_version_
                    && cached_tail_clip_ == visible_end)
                {
                    tail_unchanged = true;
                } else {
                    tail_hash = fnv1a64(tail);
                    tail_unchanged =
                        cached_tail_in_fence_ == in_code_fence_
                        && cached_tail_len_   == tail.size()
                        && cached_tail_hash_  == tail_hash;
                }
                if (!tail_unchanged) {
                    box.children.back() = render_tail(tail);
                    cached_tail_hash_     = tail_hash;
                    cached_tail_len_      = tail.size();
                    cached_tail_in_fence_ = in_code_fence_;
                }
                cached_tail_version_     = source_version_;
                cached_tail_clip_        = visible_end;
            }
            cached_prefix_gen_ = prefix_->generation;
            cached_fold_gen_   = fold_generation_;
            cached_tail_size_  = tail.size();
            build_dirty_       = false;
            return render_live_overlay_();
        }
    }

    // ── Full rebuild ──
    //
    // Either the prefix generation moved (commit_range fired), the
    // shape (has_prefix / has_tail) changed, or the cache was reset.
    // Build the outer vstack with N + (has_tail ? 1 : 0) children:
    //   children[0 .. N-1] = committed prefix blocks (deep-copied)
    //   children[last]     = render_tail() output (when has_tail)
    //
    // Why direct children instead of wrapping in a ComponentElement.
    // The earlier approach wrapped each prefix shared_ptr<const Element>
    // via the Element{sp} converting constructor, producing one
    // ComponentElement per block whose hash_id was derived from sp
    // identity. The renderer then cached per-block layout+paint cells
    // by (sp, width), making per-commit cost O(new_block_size) instead
    // of O(transcript_size). The trade was correctness: the
    // ComponentElement cache hit a cells_rows < content_h blit path
    // where the cached cell snapshot was shorter than the layout-
    // allocated content height. When the inline frame later overflowed
    // term_h and pushed rows into the terminal's native (immutable)
    // scrollback, those short cell ranges left blank rows above and
    // below the block's actual content — the "ghost rectangles" that
    // accumulate after 2-3 turns of long streaming. Direct children
    // bypass the cells cache entirely: every frame's layout walks the
    // prefix's pre-built Element values directly, producing the same
    // cells the live frame would have rendered, so the cells captured
    // into native scrollback during overflow are correct.
    //
    // Cost: O(transcript_size) deep-copies of Element variant bodies
    // per generation bump. Pricey on long sessions, but maybe_virtualize
    // bounds the relevant tail of the transcript — older turns get
    // committed to scrollback and their blocks drop out of the active
    // prefix entirely.
    std::vector<Element> outer_children;
    outer_children.reserve(
        (has_prefix ? prefix_->blocks.size() : 0u) + (has_tail ? 1u : 0u));

    if (has_prefix) {
        // Wrap the committed blocks in a single ComponentElement with a
        // generation-derived hash_id. The renderer's component_cache
        // then stores the rendered cells against this key; between
        // commits (generation stable) every render hits the cache and
        // blits cells directly, skipping the deep-copy lambda below
        // entirely. On commit the generation bumps, the hash_id
        // changes, the next render misses and re-runs the lambda once,
        // then caches again. Avoids the per-block `Element{sp}`
        // wrapper (visible blank-row bug) while still skipping the
        // per-frame copy work the previous inline-into-outer_children
        // path was paying at 100% CPU on long streams.
        outer_children.push_back(make_prefix_component());
    }
    if (has_tail) {
        outer_children.push_back(render_tail(tail));
    }

    // align_self(Stretch) forces this vstack to claim the parent's full
    // cross-axis size, regardless of how much its OWN children would
    // naturally span. Without this, early streaming frames where the
    // tail is a short inline-only paragraph (one wrapped row or less)
    // produce a vstack whose natural width = tail length = short, and
    // any parent header doing justify-content: space-between against
    // that width packs its children to the left. Setting Stretch makes
    // the streaming widget's width invariant across the
    // short-tail → multi-row-tail → committed-blocks transitions —
    // the parent's flex math sees the same number every frame, so the
    // header layout doesn't flicker between content-sized and
    // terminal-sized.
    cached_build_ = (
        detail::vstack().gap(1).padding(0, 0, 0, 2)
            .align_self(Align::Stretch)(std::move(outer_children))
    ).build();
    cached_tail_size_     = tail.size();
    cached_prefix_gen_    = prefix_->generation;
    cached_fold_gen_      = fold_generation_;
    cached_has_tail_      = has_tail;
    cached_has_prefix_    = has_prefix;
    cached_tail_in_fence_ = in_code_fence_;
    if (has_tail) {
        cached_tail_hash_ = fnv1a64(tail);
        cached_tail_len_  = tail.size();
    } else {
        cached_tail_hash_ = 0;
        cached_tail_len_  = 0;
    }
    cached_tail_version_ = source_version_;
    cached_tail_clip_    = visible_end;
    build_dirty_          = false;
    return render_live_overlay_();
}

} // namespace maya
