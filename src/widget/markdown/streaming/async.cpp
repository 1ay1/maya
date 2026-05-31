// async.cpp — StreamingMarkdown off-thread parse path.
//
// set_content_async hands large divergent parses to a detached worker
// (single-flight coalesced via async_latest_source_), keeping the previous
// frame's element tree visible until the result lands. spawn_async_worker_
// runs the full top-level parse + per-block segmentation on the worker;
// maybe_apply_async_ (foreground-only, called from build()) adopts or
// re-queues the landed result.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {

using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::collect_ref_defs;

bool StreamingMarkdown::is_parsing() const noexcept {
    // No need to lock for a coarse "in flight?" probe — the slot
    // pointer is set under the mutex and read here without; the
    // worker only flips `ready` and then the foreground destroys
    // the slot. A torn read of the shared_ptr is benign (returns
    // false negative for one frame at worst).
    return static_cast<bool>(async_slot_);
}

void StreamingMarkdown::set_content_async(std::string_view content) {
    // Same fast-path as set_content: no-op on unchanged.
    if (content.size() == source_.size() &&
        (content.empty() || std::memcmp(content.data(), source_.data(),
                                        content.size()) == 0)) {
        return;
    }
    // Pure-append growth → cheap incremental sync path is strictly
    // better than handing the delta off to a worker (the steady-state
    // streaming case). The expensive case async exists for is the
    // "divergent prefix" / large initial set path below.
    if (content.size() > source_.size() &&
        (source_.empty() || std::memcmp(content.data(), source_.data(),
                                        source_.size()) == 0)) {
        set_content(content);
        return;
    }

    // Below a threshold the synchronous reset-and-parse path is faster
    // than thread handoff; pay the cost inline. 16 KB is a generous
    // ceiling — pasting a long markdown blob comfortably exceeds it
    // while typical short composer edits stay below.
    constexpr std::size_t kAsyncThreshold = 16 * 1024;
    if (content.size() < kAsyncThreshold) {
        set_content(content);
        return;
    }

    // Divergent and large → schedule a background parse. Keep the
    // current cached_build_ visible until the worker lands so the UI
    // doesn't blank.
    std::string requested{content};
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        async_latest_source_ = requested;
        // If a worker is already in flight, just update the latest
        // request — maybe_apply_async_ will spawn a follow-up on
        // arrival of the in-flight result. This is the Zed-style
        // single-flight coalescer.
        if (async_slot_) return;
    }
    spawn_async_worker_(std::move(requested));
}

void StreamingMarkdown::spawn_async_worker_(std::string source) const {
    auto slot = std::make_shared<AsyncResult>();
    slot->source = source;
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        async_slot_ = slot;
    }

    // Detached worker: result lifetime is tied to the shared_ptr in
    // the slot, which the foreground holds the only other copy of.
    // If the StreamingMarkdown is destroyed while a worker is alive,
    // the foreground's slot copy is dropped on destruction; the
    // worker writes into its own slot copy (still alive), and the
    // result is then silently discarded when the worker's local
    // shared_ptr falls out of scope.
    std::thread([slot, src = std::move(source)]() mutable {
        // Re-parse from scratch. We can't reuse the host's incremental
        // state because that lives on the foreground thread; instead
        // we run the full top-level parse on the worker. Output is
        // the rendered Element list + per-block metadata + the
        // ref-defs map a fresh parse produces.
        std::unordered_map<std::string, md::LinkRef> defs;
        std::string cleaned = collect_ref_defs(std::string_view{src}, defs);
        ::maya::md_detail::RefDefsScope guard(&defs);
        auto parsed = parse_markdown_impl(cleaned, 0);

        // Compute segment ranges over the full src — same algorithm
        // as the foreground commit_range above, just bounded by the
        // whole buffer.
        std::vector<std::pair<std::size_t, std::size_t>> seg_ranges;
        seg_ranges.reserve(parsed.blocks.size() + 1);
        {
            bool seg_in_fence = false;
            std::size_t k = 0;
            while (k < src.size() && src[k] == '\n') ++k;
            std::size_t seg_start = k;
            while (k < src.size()) {
                bool at_ls = (k == 0 || src[k - 1] == '\n');
                if (at_ls && k + 3 <= src.size() &&
                    ((src[k] == '`' && src[k+1] == '`' && src[k+2] == '`') ||
                     (src[k] == '~' && src[k+1] == '~' && src[k+2] == '~'))) {
                    seg_in_fence = !seg_in_fence;
                    std::size_t eol = src.find('\n', k);
                    if (eol == std::string::npos) { k = src.size(); break; }
                    k = eol + 1;
                    if (!seg_in_fence) {
                        if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                        while (k < src.size() && src[k] == '\n') ++k;
                        seg_start = k;
                    }
                    continue;
                }
                if (!seg_in_fence && at_ls && src[k] == '\n') {
                    if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                    while (k < src.size() && src[k] == '\n') ++k;
                    seg_start = k;
                    continue;
                }
                ++k;
            }
            if (seg_start < src.size())
                seg_ranges.emplace_back(seg_start, src.size());
        }
        const bool seg_match = seg_ranges.size() == parsed.blocks.size();

        slot->blocks.reserve(parsed.blocks.size());
        slot->metas.reserve (parsed.blocks.size());
        std::size_t synth = 0;
        for (std::size_t bi = 0; bi < parsed.blocks.size(); ++bi) {
            auto& block = parsed.blocks[bi];
            BlockMeta meta;
            if (seg_match) {
                meta.source_offset = seg_ranges[bi].first;
                meta.source_end    = seg_ranges[bi].second;
            } else {
                meta.source_offset = synth;
                meta.source_end = (bi + 1 < parsed.blocks.size())
                    ? (synth + 1) : src.size();
                synth += 1;
            }
            meta.kind = std::visit([](const auto& x) noexcept -> BlockKind {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, md::Paragraph>)       return BlockKind::Paragraph;
                else if constexpr (std::is_same_v<T, md::Heading>)    return BlockKind::Heading;
                else if constexpr (std::is_same_v<T, md::CodeBlock>)  return BlockKind::CodeBlock;
                else if constexpr (std::is_same_v<T, md::Blockquote>) return BlockKind::Blockquote;
                else if constexpr (std::is_same_v<T, md::List>)       return BlockKind::List;
                else if constexpr (std::is_same_v<T, md::HRule>)      return BlockKind::HRule;
                else if constexpr (std::is_same_v<T, md::Table>)      return BlockKind::Table;
                else if constexpr (std::is_same_v<T, md::FootnoteDef>) return BlockKind::FootnoteDef;
                else if constexpr (std::is_same_v<T, md::Alert>)      return BlockKind::Alert;
                else if constexpr (std::is_same_v<T, md::DefList>)    return BlockKind::DefList;
                else if constexpr (std::is_same_v<T, md::Details>)    return BlockKind::Details;
                else if constexpr (std::is_same_v<T, md::HtmlBlock>)  return BlockKind::HtmlBlock;
                else return BlockKind::Other;
            }, block.inner);
            if (auto* c = std::get_if<md::CodeBlock>(&block.inner)) meta.lang = c->lang;
            std::size_t lc = 0;
            for (std::size_t q = meta.source_offset; q < meta.source_end && q < src.size(); ++q)
                if (src[q] == '\n') ++lc;
            meta.line_count = static_cast<std::uint16_t>(std::min<std::size_t>(lc, 0xFFFFu));
            slot->blocks.push_back(
                std::make_shared<const Element>(md_block_to_element(block)));
            slot->metas.push_back(std::move(meta));
        }
        slot->ref_defs = std::move(defs);
        // in_code_fence at end-of-source — count ``` / ~~~ fence
        // toggles at line starts.
        bool fence = false;
        for (std::size_t j = 0; j < src.size(); ++j) {
            bool at_ls = (j == 0 || src[j - 1] == '\n');
            if (!at_ls) continue;
            if (j + 3 <= src.size() &&
                ((src[j] == '`' && src[j+1] == '`' && src[j+2] == '`') ||
                 (src[j] == '~' && src[j+1] == '~' && src[j+2] == '~')))
                fence = !fence;
        }
        slot->in_code_fence = fence;
        // Publish: release-store on `ready` so the foreground's
        // acquire-load sees the populated vectors above.
        slot->ready.store(true, std::memory_order_release);
    }).detach();
}

void StreamingMarkdown::maybe_apply_async_() const {
    std::shared_ptr<AsyncResult> slot;
    std::optional<std::string>   latest_after;
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        if (!async_slot_) return;
        if (!async_slot_->ready.load(std::memory_order_acquire)) return;
        slot = std::move(async_slot_);  // detach from member
        latest_after = async_latest_source_;
    }

    // Decide what to do with the landed result:
    //   A. The caller's latest request matches what this worker
    //      parsed → adopt the result; clear latest_source.
    //   B. The caller has since asked for a different source →
    //      discard this result, spawn a follow-up on the new
    //      request.
    const bool current = latest_after && *latest_after == slot->source;
    if (current) {
        // Adopt. This is the foreground thread (build() calls us),
        // so mutating self's state is safe with no extra locks.
        auto self = const_cast<StreamingMarkdown*>(this);
        self->source_ = slot->source;
        self->committed_ = slot->source.size();
        self->in_code_fence_ = slot->in_code_fence;
        self->ref_defs_ = std::move(slot->ref_defs);
        self->sink_.reset();
        auto fresh = std::make_shared<CommittedPrefix>();
        fresh->blocks = std::move(slot->blocks);
        fresh->metas  = std::move(slot->metas);
        // Generation must STRICTLY exceed the prior gen so the
        // prefix ComponentElement's hash_id changes and the
        // renderer's cache misses cleanly.
        fresh->generation = self->prefix_->generation + 1;
        self->prefix_ = std::move(fresh);
        self->build_dirty_ = true;
        ++self->source_version_;
        // Reset scanner / sink state to the new committed end.
        self->scan_cursor_ = self->committed_;
        self->scan_in_fence_ = self->in_code_fence_;
        self->scan_last_boundary_ = self->committed_;
        // Drop fold map entries that no longer correspond to any
        // block in the new prefix (offsets shifted under us).
        std::vector<std::size_t> drop;
        for (auto& kv : self->folds_) {
            bool known = false;
            for (const auto& m : self->prefix_->metas)
                if (m.source_offset == kv.first) { known = true; break; }
            if (!known) drop.push_back(kv.first);
        }
        for (auto k : drop) self->folds_.erase(k);
        {
            std::lock_guard<std::mutex> lk(self->async_mu_);
            self->async_latest_source_.reset();
        }
    } else if (latest_after) {
        // Stale: spawn a follow-up for the most recent request.
        spawn_async_worker_(std::move(*latest_after));
    }
}

} // namespace maya
