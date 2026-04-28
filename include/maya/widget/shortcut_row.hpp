#pragma once
// maya::widget::ShortcutRow — width-adaptive keyboard hint row.
//
// Helix / Lazygit / k9s style: bold key in default fg, dim label,
// no chip background, no per-key color category — pro TUIs lean on
// typography weight (bold vs dim) for hierarchy. The row drops
// lower-priority bindings on narrow widths and can drop labels
// entirely (key-only mode) below a threshold.
//
//   maya::ShortcutRow{{
//       .bindings = {
//           {.key="^K", .label="palette", .priority=10},
//           {.key="^J", .label="threads", .priority=9},
//           {.key="^C", .label="quit",    .priority=10, .key_color=Color::red()},
//       },
//       .label_min_width = 110,
//       .full_min_width  = 55,
//   }}.build();

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class ShortcutRow {
public:
    // A binding is dropped when width < its `priority_min_width`. Higher
    // priority bindings stay in longer — caller assigns numeric priority
    // (any scale; only the relative order matters).
    struct Binding {
        std::string key;
        std::string label;
        Color       key_color = Color::cyan();
        int         priority  = 0;     // higher = kept longer on narrow widths
    };

    struct Config {
        std::vector<Binding> bindings;

        // Width thresholds — caller controls progressive disclosure.
        int label_min_width = 110;     // < this drops labels (key-only mode)
        int full_min_width  = 55;      // < this drops the lower-priority half

        Color text_color = Color::bright_white();
    };

    explicit ShortcutRow(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Capture by value so the lambda outlives this call.
        Config cfg = cfg_;
        return Element{ComponentElement{
            .render = [cfg = std::move(cfg)](int w, int /*h*/) -> Element {
                using namespace dsl;
                if (w <= 0) return Element{TextElement{}};

                const Color muted = Color::bright_black();
                const bool show_label = (w >= cfg.label_min_width);

                std::vector<int> kept;
                kept.reserve(cfg.bindings.size());
                if (w >= cfg.full_min_width) {
                    for (int i = 0; i < static_cast<int>(cfg.bindings.size()); ++i)
                        kept.push_back(i);
                } else {
                    // Sort by priority desc, keep top half (rounded up).
                    std::vector<int> idx(cfg.bindings.size());
                    for (std::size_t i = 0; i < idx.size(); ++i)
                        idx[i] = static_cast<int>(i);
                    std::stable_sort(idx.begin(), idx.end(),
                        [&](int a, int b) {
                            return cfg.bindings[a].priority
                                 > cfg.bindings[b].priority;
                        });
                    int keep_n = static_cast<int>((idx.size() + 1) / 2);
                    idx.resize(static_cast<std::size_t>(keep_n));
                    std::sort(idx.begin(), idx.end());
                    kept = std::move(idx);
                }

                std::vector<Element> row;
                row.reserve(kept.size() * 4 + 1);
                row.push_back(text(" "));
                bool first = true;
                for (int i : kept) {
                    const auto& b = cfg.bindings[static_cast<std::size_t>(i)];
                    if (!first) row.push_back(text("   "));   // gap
                    first = false;
                    row.push_back(text(b.key,
                                       Style{}.with_fg(cfg.text_color).with_bold()));
                    if (show_label && !b.label.empty()) {
                        row.push_back(text(" "));
                        row.push_back(text(b.label, fg_dim_(muted)));
                    }
                }
                return h(std::move(row)).build();
            },
            .layout = {},
        }};
    }

private:
    Config cfg_;

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
