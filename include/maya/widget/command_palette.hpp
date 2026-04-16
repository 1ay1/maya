#pragma once
// maya::widget::command_palette — Fuzzy-search command launcher
//
// A searchable command list with keyboard navigation. Embeds an Input widget
// for filtering and displays matched results with highlighted characters.
//
// Usage:
//   CommandPalette palette({
//       {.name = "Open File",     .description = "Browse files", .shortcut = "Ctrl+O"},
//       {.name = "Toggle Theme",  .description = "Switch dark/light"},
//   });
//   palette.on_execute([](int idx) { /* run command */ });
//   palette.show();

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"
#include "input.hpp"

namespace maya {

struct Command {
    std::string name;
    std::string description;
    std::string shortcut;
};

class CommandPalette {
    Input<> search_;
    std::vector<Command> commands_;
    std::vector<int> filtered_;    // indices into commands_
    Signal<int> cursor_{0};
    FocusNode focus_;
    bool visible_ = false;

    std::move_only_function<void(int)> on_execute_;

public:
    explicit CommandPalette(std::vector<Command> commands)
        : commands_(std::move(commands))
    {
        rebuild_filtered();
        search_.on_change([this](std::string_view) {
            rebuild_filtered();
            cursor_.set(0);
        });
    }

    // -- Accessors --
    [[nodiscard]] const Signal<int>& cursor()    const { return cursor_; }
    [[nodiscard]] FocusNode& focus_node()               { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()  const { return focus_; }
    [[nodiscard]] bool visible()                 const { return visible_; }

    // -- Visibility --
    void show() {
        visible_ = true;
        search_.clear();
        cursor_.set(0);
        rebuild_filtered();
    }

    void hide() {
        visible_ = false;
    }

    void toggle() {
        if (visible_) hide(); else show();
    }

    // -- Callback --
    template <std::invocable<int> F>
    void on_execute(F&& fn) { on_execute_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!visible_) return false;
        if (!focus_.focused()) return false;

        // Check for Escape and navigation keys first
        bool handled = std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Escape:
                        hide();
                        return true;
                    case SpecialKey::Up:
                        move_up();
                        return true;
                    case SpecialKey::Down:
                        move_down();
                        return true;
                    case SpecialKey::Enter:
                        execute_selected();
                        return true;
                    default:
                        return false;
                }
            },
            [&](CharKey) -> bool {
                return false;
            },
        }, ev.key);

        if (handled) return true;

        // Forward remaining keys to the search input
        return search_.handle(ev);
    }

    /// Forward paste events to the embedded search input.
    void handle_paste(const PasteEvent& pe) {
        if (!visible_) return;
        search_.handle_paste(pe);
    }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (!visible_) return Element{TextElement{}};

        int cur = cursor_();
        bool focused = focus_.focused();

        auto name_style    = Style{};
        auto desc_style    = Style{}.with_dim();
        auto sc_style      = Style{}.with_dim();
        auto active_name   = Style{}.with_fg(Color::blue()).with_bold();
        auto active_desc   = Style{}.with_fg(Color::blue());
        auto match_style   = Style{}.with_fg(Color::yellow()).with_bold();

        const auto& query = search_.value()();

        // Build result rows
        std::vector<Element> rows;
        rows.reserve(filtered_.size());

        for (int fi = 0; fi < static_cast<int>(filtered_.size()); ++fi) {
            int cmd_idx = filtered_[static_cast<size_t>(fi)];
            const auto& cmd = commands_[static_cast<size_t>(cmd_idx)];
            bool is_active = (fi == cur) && focused;

            // Build name with match highlights
            std::string content = "  " + cmd.name;
            std::vector<StyledRun> runs;

            if (!query.empty()) {
                // Find match positions in the name for highlighting
                auto match_positions = find_match_positions(cmd.name, query);
                size_t prefix_len = 2; // "  " prefix

                // Build runs character by character for the name portion
                size_t name_start = prefix_len;
                size_t pos = 0;
                size_t run_start = 0;
                bool in_match = false;

                // Leading "  "
                auto base_name = is_active ? active_name : name_style;

                // Process the full content with match highlighting
                runs.push_back(StyledRun{0, prefix_len, base_name});

                size_t i = 0;
                while (i < cmd.name.size()) {
                    bool is_match_char = match_positions.end() !=
                        std::find(match_positions.begin(), match_positions.end(), i);
                    size_t byte_start = name_start + i;

                    // Get UTF-8 char length
                    size_t ch_len = 1;
                    uint8_t lead = static_cast<uint8_t>(cmd.name[i]);
                    if (lead >= 0xF0) ch_len = 4;
                    else if (lead >= 0xE0) ch_len = 3;
                    else if (lead >= 0xC0) ch_len = 2;

                    if (is_match_char) {
                        runs.push_back(StyledRun{byte_start, ch_len, match_style});
                    } else {
                        runs.push_back(StyledRun{byte_start, ch_len, base_name});
                    }
                    i += ch_len;
                }
            } else {
                runs.push_back(StyledRun{0, content.size(),
                    is_active ? active_name : name_style});
            }

            // Append description
            if (!cmd.description.empty()) {
                size_t desc_start = content.size();
                content += "  " + cmd.description;
                runs.push_back(StyledRun{desc_start, content.size() - desc_start,
                    is_active ? active_desc : desc_style});
            }

            // Append shortcut
            if (!cmd.shortcut.empty()) {
                size_t sc_start = content.size();
                content += "    " + cmd.shortcut;
                runs.push_back(StyledRun{sc_start, content.size() - sc_start, sc_style});
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = name_style,
                .runs = std::move(runs),
            }});
        }

        if (filtered_.empty()) {
            rows.push_back(Element{TextElement{
                .content = "  No matching commands",
                .style = Style{}.with_dim(),
            }});
        }

        auto results = dsl::v(std::move(rows)).build();

        // Wrap search + results in a bordered box
        auto border_color = focused
            ? Color::blue()
            : Color::bright_black();

        return (dsl::v(
                search_.build(),
                Element{ComponentElement{
                    .render = [](int w, int /*h*/) -> Element {
                        std::string line;
                        for (int i = 0; i < w; ++i) line += "\xe2\x94\x80"; // "─"
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = Style{}.with_fg(Color::bright_black()),
                        }};
                    },
                    .layout = {},
                }},
                std::move(results)
            ) | dsl::border(BorderStyle::Round) | dsl::bcolor(border_color)
              | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    void rebuild_filtered() {
        filtered_.clear();
        const auto& query = search_.value()();

        for (int i = 0; i < static_cast<int>(commands_.size()); ++i) {
            if (query.empty() || substring_match(commands_[static_cast<size_t>(i)].name, query)) {
                filtered_.push_back(i);
            }
        }
    }

    static bool substring_match(const std::string& haystack, const std::string& needle) {
        // Case-insensitive substring match
        if (needle.size() > haystack.size()) return false;
        auto to_lower = [](char c) -> char {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        };
        for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            bool found = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                if (to_lower(haystack[i + j]) != to_lower(needle[j])) {
                    found = false;
                    break;
                }
            }
            if (found) return true;
        }
        return false;
    }

    /// Returns byte offsets within `name` where the substring match begins.
    static std::vector<size_t> find_match_positions(
        const std::string& name, const std::string& query)
    {
        std::vector<size_t> positions;
        if (query.empty()) return positions;

        auto to_lower = [](char c) -> char {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        };

        // Find first substring match position
        for (size_t i = 0; i + query.size() <= name.size(); ++i) {
            bool found = true;
            for (size_t j = 0; j < query.size(); ++j) {
                if (to_lower(name[i + j]) != to_lower(query[j])) {
                    found = false;
                    break;
                }
            }
            if (found) {
                for (size_t j = 0; j < query.size(); ++j)
                    positions.push_back(i + j);
                break;
            }
        }
        return positions;
    }

    void move_up() {
        int c = cursor_();
        int n = static_cast<int>(filtered_.size());
        if (n == 0) return;
        cursor_.set(c > 0 ? c - 1 : n - 1);
    }

    void move_down() {
        int c = cursor_();
        int n = static_cast<int>(filtered_.size());
        if (n == 0) return;
        cursor_.set((c + 1) % n);
    }

    void execute_selected() {
        if (filtered_.empty()) return;
        int fi = cursor_();
        if (fi >= 0 && fi < static_cast<int>(filtered_.size())) {
            int cmd_idx = filtered_[static_cast<size_t>(fi)];
            hide();
            if (on_execute_) on_execute_(cmd_idx);
        }
    }
};

} // namespace maya
