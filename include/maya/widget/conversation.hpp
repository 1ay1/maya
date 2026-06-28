#pragma once
// maya::widget::Conversation — vertical list of turns with dim dividers
// between them and an optional trailing in-flight indicator.
//
// Owned:
//   - Width-aware dim ─ separator between consecutive turns.
//   - Trailing in-flight slot.
//   - Outer padding(0,1) + grow(1.0f) so the panel fills its column.
//
// NOT owned:
//   - Per-turn rendering — caller passes pre-built turn Elements
//     (typically maya::Turn instances).
//   - Empty-state / welcome — caller decides what to render when the
//     list is empty.
//
//   maya::Conversation{{
//       .turns     = built_turn_elements,
//       .in_flight = activity_indicator_element,    // optional
//   }}.build();

#include <optional>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include "activity_indicator.hpp"
#include "turn.hpp"

namespace maya {

class Conversation {
public:
    // A pre-built turn Element + the continuation flag. The host
    // (moha) maintains its own (thread, message) → Element cache and
    // hands settled turns straight through as Element values, so a
    // long session's per-frame cost stays O(visible_live_turns)
    // instead of O(visible_total_turns × tool_cards_per_turn). This
    // is the same pattern the agent_session example uses with
    // `m.frozen` + `list_ref` — settled rendering is computed once
    // per turn lifetime, not once per frame. The continuation bit
    // drives the inter-turn rule below so callers don't have to
    // recreate that layout policy in their host.
    struct PreBuilt {
        Element element;
        bool    continuation = false;
    };

    struct Config {
        // Original config-based path. Kept for callers that haven't
        // adopted the Element cache yet — every Turn::Config in this
        // vector gets `Turn{cfg}.build()` called on it once per frame.
        std::vector<Turn::Config>                turns;
        // Middle path: when non-empty, takes precedence over `turns`.
        // Each entry is a fully-built turn Element; build() lays them
        // out with dividers between non-continuation entries.
        //
        // Per-frame cost: build() copies each `element` into the rows
        // vector once. To make the ComponentElements inside survive
        // those copies, hosts that want render-cache-stable settled
        // turns should hand in Elements whose ComponentElement
        // wrappers carry a `hash_id` (see element.hpp). Maya keeps
        // the rendered result indexed by hash rather than by pointer,
        // so the freshly-copied wrapper at this slot still hits the
        // cache produced by the original.
        std::vector<PreBuilt>                    built_turns;

        // Fastest path (agent_session pattern). When `frozen != nullptr`,
        // takes precedence over both `built_turns` and `turns`.
        //
        // `frozen` is a BORROWED pointer to the host's append-only
        // vector of fully-built scrollback Elements (one row per entry:
        // turn body, or a divider/gap pushed by the host). build()
        // hands maya a `list_ref(*frozen)` — zero-copy across frames,
        // regardless of how large the prefix grows. Every Element
        // inside benefits from maya's hash_id-keyed layout + cell
        // cache, so per-frame cost stays O(visible_live) instead of
        // O(total_turns).
        //
        // The host owns inter-turn dividers: push them into `*frozen`
        // at the moments you want them (typically a one-row gap
        // before each fresh-speaker turn). This widget adds NO
        // dividers when the frozen path is active — what you see is
        // exactly the rows in `*frozen`, followed by `live_tail` (if
        // non-empty), followed by `in_flight` (if set).
        //
        // Lifetime: the pointer must outlive build(). Typical use is
        // a member of the host's model that lives across frames.
        const std::vector<Element>*              frozen           = nullptr;

        // The live tail — one entry per in-flight or unfrozen Turn.
        // Rendered after the frozen prefix in order. The host is
        // responsible for any leading divider/gap between the last
        // frozen row and the first live entry — push that into
        // `*frozen` at the time it freezes the previous turn, OR
        // prepend a gap as the first `live_tail` entry. Same rule
        // for between live entries: include the gap yourself.
        std::vector<Element>                     live_tail;

        std::optional<ActivityIndicator::Config> in_flight;

        // Viewport-fill discipline. true (default) = the classic
        // monolithic inline shape: a trailing flex spacer + grow(1.0f)
        // so the panel fills its column and the chrome below (composer +
        // status bar) rides the viewport bottom on short content.
        //
        // false = HUG mode, for the depositional (strata) renderer. The
        // settled prefix is composed as separate sealed nodes ABOVE this
        // one, so this node must hug its own live-tail content height —
        // a trailing grow-spacer here would inflate the live node to the
        // whole viewport and strand a blank void between the settled
        // turns and the composer. No spacer, no grow: the composer sits
        // directly under the live content.
        bool                                     fill_viewport = true;
    };

    explicit Conversation(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // ── Fastest path: borrowed frozen prefix + live tail. ─────
        // The host owns dividers inside *frozen and between frozen
        // and live_tail; we just concatenate.
        if (cfg_.frozen != nullptr) {
            std::vector<Element> rows;
            rows.reserve(cfg_.live_tail.size() + 3);
            rows.push_back(list_ref(*cfg_.frozen));
            for (const auto& e : cfg_.live_tail) rows.push_back(e);
            if (cfg_.in_flight)
                rows.push_back(ActivityIndicator{*cfg_.in_flight}.build());
            // Trailing flex spacer absorbs the leftover vertical space
            // so the last Turn doesn't stretch its rail downward into
            // the gap above the composer. (The composer anti-bounce pad
            // lives in AppLayout::build as the vstack's LAST child — a
            // pad here would be absorbed by this spacer's grow and never
            // reach content_height.)
            //
            // HUG mode (fill_viewport=false, strata path): skip the
            // spacer + grow so the node hugs the live-tail height, and
            // drop the horizontal padding so live turns sit flush-left
            // at col 0 — byte-aligned with the sealed nodes above, which
            // render bare at the full terminal width. A 1-col pad here
            // would shift the live turn right of its own sealed copy.
            if (!cfg_.fill_viewport)
                return v(rows).build();
            rows.push_back(spacer().build());
            return (v(rows) | padding(0, 1) | grow(1.0f)).build();
        }

        std::vector<Element> rows;
        const bool use_built = !cfg_.built_turns.empty();
        const std::size_t n = use_built ? cfg_.built_turns.size()
                                        : cfg_.turns.size();
        rows.reserve(n * 2 + cfg_.live_tail.size() + 2);
        for (std::size_t i = 0; i < n; ++i) {
            // Skip the inter-turn rule before a continuation turn so the
            // rail flows uninterrupted through a same-speaker run.
            const bool continuation = use_built ? cfg_.built_turns[i].continuation
                                                : cfg_.turns[i].continuation;
            if (i > 0 && !continuation)
                rows.push_back(divider_rule());
            rows.push_back(use_built ? cfg_.built_turns[i].element
                                     : Turn{cfg_.turns[i]}.build());
        }
        // Live tail. In the strata path the host hands the settled prefix
        // to maya as separate sealed NODES (frozen == nullptr here) and
        // fills `live_tail` with the in-flight turn + chrome; this branch
        // must render it or the live node paints empty (the streaming
        // void: nothing shows mid-turn, the whole turn pops in at once
        // when it settles into the sealed prefix). The host owns any
        // leading divider/gap inside `live_tail`.
        for (const auto& e : cfg_.live_tail) rows.push_back(e);
        if (cfg_.in_flight)
            rows.push_back(ActivityIndicator{*cfg_.in_flight}.build());
        // HUG mode (fill_viewport=false, strata path): skip the spacer +
        // grow so the live node hugs its content and the composer sits
        // directly beneath it (no blank void above the chrome), and drop
        // the horizontal padding so live turns are flush-left at col 0,
        // byte-aligned with the bare sealed nodes above (else the turn
        // visibly shifts left + widens the instant it seals).
        if (!cfg_.fill_viewport)
            return v(rows).build();
        rows.push_back(spacer().build());
        return (v(rows) | padding(0, 1) | grow(1.0f)).build();
    }

    // Build the standard width-aware dim inter-turn rule. Exposed so
    // hosts using the `frozen` path can push the same divider Element
    // into their frozen vector at turn boundaries, matching the look
    // of the legacy auto-dividers from the built_turns path.
    [[nodiscard]] static Element divider() { return divider_rule(); }

private:
    Config cfg_;

    // Width-aware thin dim rule, indented 3 columns. The "next turn
    // starts here" handhold without the heaviness of a full-weight rule.
    //
    // Stable hash_id: every divider at a given terminal width renders
    // to byte-identical cells, so all N dividers in a long frozen
    // prefix can share one cached cells-blit. The renderer's hash
    // cache stores (cells, width) per entry and rejects width
    // mismatches automatically, so a single id is correct across
    // resizes (resize misses, re-paints, re-caches).
    static Element divider_rule() {
        using namespace dsl;
        return component([](int w, int /*h*/) -> Element {
            if (w <= 0) return blank().build();
            std::string line;
            constexpr int kIndent = 3;
            for (int i = 0; i < kIndent; ++i) line += ' ';
            for (int i = kIndent; i < w; ++i) line += "\xe2\x94\x80"; // ─
            return text(std::move(line),
                        Style{}.with_fg(Color::bright_black()).with_dim()).build();
        })
        .hash_id(CacheIdBuilder{}
            .add(std::string_view{"maya.conversation.divider"})
            .build());
    }
};

} // namespace maya
