#pragma once
// maya::widget::key_help — Keyboard shortcut reference panel
//
// Displays grouped key bindings in a bordered box. Uses ComponentElement
// to adapt between 1-column and 2-column layouts based on available width.
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

    // Find max key width for alignment
    [[nodiscard]] int max_key_width() const {
        int max_w = 0;
        for (const auto& b : bindings_) {
            max_w = std::max(max_w, static_cast<int>(b.key.size()));
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
                .style = Style{}.with_fg(Color::rgb(92, 99, 112)),
            }};
        }

        auto grps = groups();
        int key_w = max_key_width();

        // Capture everything by value to avoid dangling this pointer
        auto bindings_copy = bindings_;
        std::string title = title_;

        Element content = Element{ComponentElement{
            .render = [grps, key_w, bindings_copy, title](int width, int /*height*/) -> Element {
                // Estimate single-column width: 4 (indent) + key_w + 2 + max_desc
                int col_width = key_w + 6;
                for (const auto& b : bindings_copy) {
                    col_width = std::max(col_width,
                        4 + key_w + 2 + static_cast<int>(b.description.size()));
                }

                bool two_columns = (width >= col_width * 2 + 4);

                // Helper: build column from a subset of groups
                auto make_column = [&](const std::vector<std::string>& col_grps) -> Element {
                    auto group_style = Style{}.with_bold().with_fg(Color::rgb(200, 204, 212));
                    auto k_style     = Style{}.with_bold().with_fg(Color::rgb(97, 175, 239));
                    auto desc_style  = Style{}.with_fg(Color::rgb(171, 178, 191));

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
                        for (const auto& b : bindings_copy) {
                            if (b.group != grp) continue;
                            std::string line = "    ";
                            std::vector<StyledRun> runs;
                            size_t key_start = line.size();
                            line += b.key;
                            int pad = key_w - static_cast<int>(b.key.size());
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

                auto wrap_border = [&](Element inner) -> Element {
                    auto node = dsl::v(std::move(inner))
                        | dsl::border(BorderStyle::Round)
                        | dsl::bcolor(Color::rgb(50, 54, 62))
                        | dsl::padding(0, 1, 0, 1);
                    if (!title.empty()) {
                        node = std::move(node) | dsl::btext(" " + title + " ",
                            BorderTextPos::Top, BorderTextAlign::Start);
                    }
                    return std::move(node).build();
                };

                if (!two_columns || grps.size() < 2) {
                    return wrap_border(make_column(grps));
                }

                // Two columns: split groups roughly in half
                size_t half = (grps.size() + 1) / 2;
                std::vector<std::string> left_grps(grps.begin(), grps.begin() + static_cast<long>(half));
                std::vector<std::string> right_grps(grps.begin() + static_cast<long>(half), grps.end());

                auto cols = dsl::h(make_column(left_grps), make_column(right_grps)).build();
                return wrap_border(std::move(cols));
            },
        }};

        return content;
    }
};

} // namespace maya
