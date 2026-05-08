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
        // Fast path: when non-empty, takes precedence over `turns`.
        // Each entry is a fully-built turn Element; the build()
        // method just lays them out with dividers between
        // non-continuation entries — no per-turn Element
        // reconstruction.
        std::vector<PreBuilt>                    built_turns;
        std::optional<ActivityIndicator::Config> in_flight;
    };

    explicit Conversation(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        std::vector<Element> rows;
        const bool use_built = !cfg_.built_turns.empty();
        const std::size_t n = use_built ? cfg_.built_turns.size()
                                        : cfg_.turns.size();
        rows.reserve(n * 2 + 1);
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
        if (cfg_.in_flight)
            rows.push_back(ActivityIndicator{*cfg_.in_flight}.build());
        return (v(rows) | padding(0, 1) | grow(1.0f)).build();
    }

private:
    Config cfg_;

    // Width-aware thin dim rule, indented 3 columns. The "next turn
    // starts here" handhold without the heaviness of a full-weight rule.
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
        });
    }
};

} // namespace maya
