#pragma once
// maya::widget::key_help — Keyboard shortcut reference panel
//
// Displays grouped key bindings in a bordered box. Responsive via maya's
// pick() (ViewThatFits): the two-column arrangement renders when its REAL
// measured width fits the slot, otherwise the single column does — no
// byte-count estimates, no breakpoints.
//
//   ╭─ Keyboard Shortcuts ─────────────────╮
//   │  Navigation                           │
//   │    Up/Down   Move cursor              │
//   │    Enter     Select item              │
//   │                                       │
//   │  Editing                              │
//   │    Ctrl+S    Save file                │
//   │    Ctrl+Z    Undo                     │
//   ╰───────────────────────────────────────╯
//
// Usage:
//   KeyHelp help;
//   help.add("Up/Down", "Move cursor", "Navigation");
//   help.add("Enter", "Select item", "Navigation");
//   auto ui = help.build();

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// KeyBinding — a single shortcut entry
// ============================================================================

struct KeyBinding {
    std::string key;
    std::string description;
    std::string group;
};

// ============================================================================
// KeyHelp — keyboard shortcut reference panel widget
// ============================================================================

class KeyHelp {
    std::vector<KeyBinding> bindings_;
    std::string title_ = "Keyboard Shortcuts";

    // Collect unique groups in insertion order
    [[nodiscard]] std::vector<std::string> groups() const {
        std::vector<std::string> out;
        for (const auto& b : bindings_) {
            bool found = false;
            for (const auto& g : out) {
                if (g == b.group) { found = true; break; }
            }
            if (!found) out.push_back(b.group);
        }
        return out;
    }

    // Find max key width for alignment (display cells, not bytes — "↑↓"
    // is 6 bytes but 2 cells).
    [[nodiscard]] int max_key_width() const {
        int max_w = 0;
        for (const auto& b : bindings_) {
            max_w = std::max(max_w, string_width(b.key));
        }
        return max_w;
    }

public:
    KeyHelp() = default;

    explicit KeyHelp(std::vector<KeyBinding> bindings)
        : bindings_(std::move(bindings)) {}

    // -- Mutation --
    void add(std::string key, std::string description, std::string group = "") {
        bindings_.push_back(KeyBinding{
            std::move(key), std::move(description), std::move(group)});
    }

    void set_title(std::string_view t) { title_ = std::string{t}; }

    void clear() { bindings_.clear(); }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (bindings_.empty()) {
            return Element{TextElement{
                .content = "  No shortcuts defined",
                .style = Style{}.with_dim(),
            }};
        }

        auto grps = groups();
        int key_w = max_key_width();

        // Helper: build a column from a subset of groups.
        auto make_column = [key_w, this](const std::vector<std::string>& col_grps) -> Element {
            auto group_style = Style{}.with_bold();
            auto k_style     = Style{}.with_bold().with_fg(Color::blue());
            auto desc_style  = Style{};

            std::vector<Element> rows;
            for (size_t gi = 0; gi < col_grps.size(); ++gi) {
                const auto& grp = col_grps[gi];
                if (gi > 0) rows.push_back(Element{TextElement{.content = ""}});
                if (!grp.empty()) {
                    rows.push_back(Element{TextElement{
                        .content = "  " + grp,
                        .style = group_style,
                        .wrap = TextWrap::NoWrap,
                    }});
                }
                for (const auto& b : bindings_) {
                    if (b.group != grp) continue;
                    std::string line = "    ";
                    std::vector<StyledRun> runs;
                    size_t key_start = line.size();
                    line += b.key;
                    int pad = key_w - string_width(b.key);
                    if (pad > 0) line.append(static_cast<size_t>(pad), ' ');
                    runs.push_back(StyledRun{key_start, b.key.size(), k_style});
                    line += "  ";
                    size_t desc_start = line.size();
                    line += b.description;
                    runs.push_back(StyledRun{desc_start, b.description.size(), desc_style});
                    rows.push_back(Element{TextElement{
                        .content = std::move(line),
                        .style = {},
                        .wrap = TextWrap::NoWrap,
                        .runs = std::move(runs),
                    }});
                }
            }
            return dsl::v(std::move(rows)).build();
        };

        auto wrap_border = [this](Element inner) -> Element {
            auto node = dsl::v(std::move(inner))
                | dsl::border(BorderStyle::Round)
                | dsl::bcolor(Color::bright_black())
                | dsl::padding(0, 1, 0, 1);
            if (!title_.empty()) {
                node = std::move(node) | dsl::btext(" " + title_ + " ",
                    BorderTextPos::Top, BorderTextAlign::Start);
            }
            return std::move(node).build();
        };

        Element one_col = make_column(grps);
        if (grps.size() < 2) return wrap_border(std::move(one_col));

        // Two columns: split groups roughly in half. pick() (ViewThatFits)
        // measures the REAL two-column fragment against the slot inside the
        // border — it renders when it truly fits, else the single column
        // fallback does. No estimated widths to drift.
        size_t half = (grps.size() + 1) / 2;
        std::vector<std::string> left_grps(grps.begin(), grps.begin() + static_cast<long>(half));
        std::vector<std::string> right_grps(grps.begin() + static_cast<long>(half), grps.end());
        Element two_col = dsl::h(make_column(left_grps),
                                 make_column(right_grps)).build();

        return wrap_border(
            detail::pick({std::move(two_col), std::move(one_col)}));
    }
};

} // namespace maya
