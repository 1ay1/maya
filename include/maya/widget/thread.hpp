#pragma once
// maya::widget::Thread — top-level conversation viewport.
//
// Owns the empty-vs-populated branch and the in-flight indicator
// decision:
//
//   - Empty thread → maya::WelcomeScreen (brand splash, starters, hints)
//   - Non-empty    → maya::Conversation (list of turns + dim dividers)
//                    plus an optional bottom maya::ActivityIndicator
//                    when the model is active and the active turn has
//                    no Timeline showing yet.
//
// All caller-side data flows through nested widget Configs — no
// Element construction in the host app:
//
//   maya::Thread{{
//       .is_empty = m.d.current.messages.empty(),
//       .welcome  = welcome_cfg(m),                  // used when is_empty
//       .turns    = build_turn_configs(m),           // used when !is_empty
//       .in_flight = optional<ActivityIndicator::Config>{...},
//   }}.build();

#include <optional>
#include <utility>
#include <vector>

#include "../element/element.hpp"

#include "activity_indicator.hpp"
#include "conversation.hpp"
#include "turn.hpp"
#include "welcome_screen.hpp"

namespace maya {

class Thread {
public:
    struct Config {
        // Empty-state branch
        bool                    is_empty = false;
        WelcomeScreen::Config   welcome;

        // Non-empty branch — one Turn::Config per visible message.
        std::vector<Turn::Config> turns;

        // Optional bottom-of-thread "still working…" indicator. Use
        // when the model is mid-stream and the active turn has no
        // visible Timeline / spinner of its own (otherwise duplicate).
        std::optional<ActivityIndicator::Config> in_flight;
    };

    explicit Thread(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (cfg_.is_empty) return WelcomeScreen{cfg_.welcome}.build();

        Conversation::Config conv;
        conv.turns.reserve(cfg_.turns.size());
        for (const auto& tc : cfg_.turns)
            conv.turns.push_back(Turn{tc}.build());

        if (cfg_.in_flight) {
            conv.in_flight = ActivityIndicator{*cfg_.in_flight}.build();
            conv.has_in_flight = true;
        }
        return Conversation{std::move(conv)}.build();
    }

private:
    Config cfg_;
};

} // namespace maya
