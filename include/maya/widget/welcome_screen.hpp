#pragma once
// maya::widget::WelcomeScreen — empty-thread brand splash + orientation.
//
// Quiet brand presence at the top of an empty conversation: an ASCII
// wordmark (bold for all rows except the last, which is dim — the
// classic CLI wordmark "depth" trick), a tagline, a center chip row
// with the active model + profile, a bordered starter-prompts card,
// and a bottom hint row of keyboard shortcuts.
//
//   maya::WelcomeScreen{{
//       .wordmark       = {"┌┬┐┌─┐┬ ┬┌─┐", "││││ │├─┤├─┤", "┴ ┴└─┘┴ ┴┴ ┴"},
//       .wordmark_color = Color::magenta(),
//       .tagline        = "a calm middleware between you and the model",
//       .model_badge    = ModelBadge{"claude-opus-4-7"}.build(),
//       .profile_label  = "write",
//       .profile_color  = Color::magenta(),
//       .starters       = {"Implement a small feature",
//                          "Refactor or clean up this file"},
//       .hints          = {{"^K", " palette", Color::cyan()},
//                          {"^J", " threads", Color::cyan()}},
//   }}.build();

#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class WelcomeScreen {
public:
    struct Hint {
        std::string key;        // e.g. "^K"
        std::string label;      // e.g. " palette" — caller controls leading space
        Color       key_color = Color::cyan();
    };

    struct Config {
        std::vector<std::string> wordmark;        // typically 3 rows of box-drawing
        Color                    wordmark_color = Color::magenta();
        std::string              tagline;

        // Center chip row: caller-built model badge sits beside a profile chip
        Element                  model_badge;       // default-empty
        std::string              profile_label;   // raw — widget renders verbatim
        Color                    profile_color = Color::magenta();

        // Starters card
        std::string              starters_title = "Try";
        std::vector<std::string> starters;

        // Bottom hint row
        std::string              hint_intro = "type to begin";
        std::vector<Hint>        hints;

        Color                    accent_color = Color::magenta();
        Color                    text_color   = Color::bright_white();
    };

    explicit WelcomeScreen(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const Color muted = Color::bright_black();

        auto centered = [](Element e) {
            return h(spacer(), std::move(e), spacer()).build();
        };
        auto centered_text = [&](std::string s, Style st) {
            return centered(text(std::move(s), st));
        };

        // Wordmark — bold for every row except the last, which uses
        // dim accent (gives the glyph block a sense of "shadow").
        std::vector<Element> wm_rows;
        wm_rows.reserve(cfg_.wordmark.size());
        for (std::size_t i = 0; i < cfg_.wordmark.size(); ++i) {
            const bool last = (i + 1 == cfg_.wordmark.size());
            Style st = last
                ? fg_dim_(cfg_.wordmark_color)
                : Style{}.with_fg(cfg_.wordmark_color).with_bold();
            wm_rows.push_back(centered_text(cfg_.wordmark[i], st));
        }
        auto wordmark = v(wm_rows);

        auto tagline = centered_text(cfg_.tagline,
                                     Style{}.with_fg(muted).with_italic());

        // Profile chip: ▌ INVERSE-LABEL ▐ — same shape moha's status bar uses.
        auto profile_chip = h(
            text("\xe2\x96\x8c", Style{}.with_fg(cfg_.profile_color)),    // ▌
            text(" " + cfg_.profile_label + " ",
                 Style{}.with_fg(cfg_.profile_color).with_inverse().with_bold()),
            text("\xe2\x96\x90", Style{}.with_fg(cfg_.profile_color))     // ▐
        );
        auto chips_row = h(
            spacer(),
            cfg_.model_badge,
            text("    "),
            profile_chip,
            spacer());

        // Starters card.
        std::vector<Element> starter_rows;
        starter_rows.push_back(text(" " + small_caps_(cfg_.starters_title) + " ",
                                    Style{}.with_fg(muted).with_bold()));
        starter_rows.push_back(blank());
        for (const auto& s : cfg_.starters) {
            starter_rows.push_back(h(
                text("\xe2\x80\xa2 ", fg_dim_(cfg_.accent_color)),         // •
                text(s, fg_dim_(cfg_.text_color))
            ).build());
        }
        auto starters_card = (v(starter_rows)
                              | padding(0, 2, 0, 2)
                              | border(BorderStyle::Round)
                              | bcolor(muted)).build();
        auto starters_row = h(spacer(), starters_card, spacer());

        // Bottom hint row: intro · key label · key label · …
        std::vector<Element> hint_parts;
        hint_parts.push_back(spacer());
        hint_parts.push_back(text(cfg_.hint_intro, fg_dim_(muted)));
        for (const auto& hint : cfg_.hints) {
            hint_parts.push_back(text("  \xc2\xb7  ", fg_dim_(muted)));
            hint_parts.push_back(text(hint.key,
                                      Style{}.with_fg(hint.key_color).with_bold()));
            hint_parts.push_back(text(hint.label, fg_dim_(muted)));
        }
        hint_parts.push_back(spacer());
        auto hint = h(hint_parts);

        return (v(
            blank(), blank(),
            wordmark,
            blank(),
            tagline,
            blank(), blank(),
            chips_row,
            blank(), blank(),
            starters_row,
            blank(), blank(),
            hint
        ) | padding(0, 1) | grow(1.0f)).build();
    }

private:
    Config cfg_;

    static std::string small_caps_(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            out.push_back(static_cast<char>(
                (c >= 'a' && c <= 'z') ? (c - 32) : c));
            if (i + 1 < s.size()) out.push_back(' ');
        }
        return out;
    }

    static Style fg_dim_(Color c) {
        const bool is_already_muted =
            c.kind() == Color::Kind::Named
            && c.index() == static_cast<uint8_t>(AnsiColor::BrightBlack);
        return is_already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
