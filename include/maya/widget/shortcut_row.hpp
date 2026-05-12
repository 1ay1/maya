#pragma once
// maya::widget::ShortcutRow — width-adaptive keyboard hint row.
//
// Helix / Lazygit / k9s style: bold key in default fg, dim label,
// no chip background, no per-key color category — pro TUIs lean on
// typography weight (bold vs dim) for hierarchy.
//
// The row greedily fits the available width:
//
//   wide   → "  ^K palette   ^J threads   ^T todo   S-Tab profile   ^/ models   ^N new   ^C quit"
//   narrow → "  ^K palette   ^J threads   ^T todo   S-Tab   ^/   ^N new   ^C quit"
//   narrower → "  ^K   ^J   ^T   S-Tab   ^/   ^N   ^C"
//   very narrow → "  ^K   ^J   ^T   ^N   ^C"   (lowest-priority bindings drop)
//
// Degradation is two-phase: first drop labels in priority-ascending order
// (lowest priority loses its label first), then drop entire bindings in
// the same order. The leftmost-highest-priority binding is never dropped.
//
//   maya::ShortcutRow{{
//       .bindings = {
//           {.key="^K", .label="palette", .priority=10},
//           {.key="^J", .label="threads", .priority=9},
//           {.key="^C", .label="quit",    .priority=10, .key_color=Color::red()},
//       },
//   }}.build();

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
    // Higher priority = kept (with its label) longer on narrow widths.
    // Numeric scale is arbitrary — only relative ordering matters.
    struct Binding {
        std::string key;
        std::string label;
        Color       key_color = Color::cyan();
        int         priority  = 0;
    };

    struct Config {
        std::vector<Binding> bindings;
        Color                text_color = Color::bright_white();
    };

    explicit ShortcutRow(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        return component([cfg = cfg_](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0 || cfg.bindings.empty()) return blank().build();

            const Color muted = Color::bright_black();

            // Keys and labels in this widget are ASCII (^K, S-Tab, palette,
            // …), so byte length equals display width — no Unicode width
            // helper needed. The notification-takeover path uses a "▎" or
            // "▎⚠" glyph in `key`, but those rows are single-binding and
            // measured loosely; an over-count of 1–2 cells is harmless.
            const int n = static_cast<int>(cfg.bindings.size());
            struct Slot {
                int  key_w;
                int  label_w;
                bool show_label;
                bool visible;
                int  priority;
            };
            std::vector<Slot> slots(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                const auto& b = cfg.bindings[static_cast<std::size_t>(i)];
                slots[i].key_w      = static_cast<int>(b.key.size());
                slots[i].label_w    = static_cast<int>(b.label.size());
                slots[i].show_label = !b.label.empty();
                slots[i].visible    = true;
                slots[i].priority   = b.priority;
            }

            constexpr int kSep  = 3;   // "   " between bindings
            constexpr int kLead = 1;   // leading " "

            auto measure = [&]() {
                int total = kLead;
                bool first = true;
                for (const auto& s : slots) {
                    if (!s.visible) continue;
                    if (!first) total += kSep;
                    first = false;
                    total += s.key_w;
                    if (s.show_label) total += 1 + s.label_w;
                }
                return total;
            };

            // Pick the next slot to degrade: lowest priority first;
            // on priority ties, drop the rightmost (visually less prominent).
            auto pick_victim = [&](bool labels_phase) -> int {
                int best = -1;
                for (int i = 0; i < n; ++i) {
                    if (!slots[i].visible) continue;
                    if (labels_phase && !slots[i].show_label) continue;
                    if (best == -1) { best = i; continue; }
                    const int pa = slots[i].priority;
                    const int pb = slots[best].priority;
                    if (pa < pb || (pa == pb && i > best)) best = i;
                }
                return best;
            };

            // Phase 1: shed labels until the row fits or all are gone.
            while (measure() > w) {
                int v = pick_victim(/*labels_phase=*/true);
                if (v < 0) break;
                slots[v].show_label = false;
            }

            // Phase 2: shed entire bindings, keeping at least the last
            // remaining one (something is more informative than nothing
            // even if it overflows by a cell).
            while (measure() > w) {
                int visible_count = 0;
                for (const auto& s : slots) if (s.visible) ++visible_count;
                if (visible_count <= 1) break;
                int v = pick_victim(/*labels_phase=*/false);
                if (v < 0) break;
                slots[v].visible = false;
            }

            std::vector<Element> row;
            row.reserve(static_cast<std::size_t>(n) * 4 + 1);
            row.push_back(text(" "));
            bool first = true;
            for (int i = 0; i < n; ++i) {
                const auto& s = slots[static_cast<std::size_t>(i)];
                if (!s.visible) continue;
                const auto& b = cfg.bindings[static_cast<std::size_t>(i)];
                if (!first) row.push_back(text("   "));
                first = false;
                row.push_back(text(b.key,
                                   Style{}.with_fg(cfg.text_color).with_bold()));
                if (s.show_label && !b.label.empty()) {
                    row.push_back(text(" "));
                    row.push_back(text(b.label, fg_dim_(muted)));
                }
            }
            return h(std::move(row)).build();
        });
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
