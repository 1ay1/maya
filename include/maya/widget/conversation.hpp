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

#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class Conversation {
public:
    struct Config {
        std::vector<Element> turns;
        Element              in_flight;          // empty Element = no indicator
        bool                 has_in_flight = false;
    };

    explicit Conversation(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        std::vector<Element> rows;
        rows.reserve(cfg_.turns.size() * 2 + 1);
        for (std::size_t i = 0; i < cfg_.turns.size(); ++i) {
            if (i > 0) rows.push_back(divider_rule());
            rows.push_back(cfg_.turns[i]);
        }
        if (cfg_.has_in_flight) rows.push_back(cfg_.in_flight);
        return (v(rows) | padding(0, 1) | grow(1.0f)).build();
    }

private:
    Config cfg_;

    // Width-aware thin dim rule, indented 3 columns. The "next turn
    // starts here" handhold without the heaviness of a full-weight rule.
    static Element divider_rule() {
        return Element{ComponentElement{
            .render = [](int w, int /*h*/) -> Element {
                std::string line;
                constexpr int kIndent = 3;
                for (int i = 0; i < kIndent; ++i) line += ' ';
                for (int i = kIndent; i < w; ++i) line += "\xe2\x94\x80"; // ─
                return Element{TextElement{
                    .content = std::move(line),
                    .style   = Style{}.with_fg(Color::bright_black()).with_dim(),
                }};
            },
            .layout = {},
        }};
    }
};

} // namespace maya
