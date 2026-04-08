#pragma once
// maya::widget::list — Scrollable selectable list with optional filtering
//
// Arrow-key navigable list of items with icon, label, and description.
// Supports filtering via '/' key and scroll windowing.
//
//   ▸ +  Add new item      Create a new entry
//     -  Remove item       Delete selected entry
//     *  Edit item         Modify selected entry
//
//   3 items
//
// Usage:
//   List menu({{"Add new item", "Create a new entry", "+"},
//              {"Remove item", "Delete selected entry", "-"}});
//   menu.on_select([](int idx, std::string_view label) { ... });
//   auto ui = menu.build();

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
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// ListItem — a single entry in the list
// ============================================================================

struct ListItem {
    std::string label;
    std::string description;  // optional
    std::string icon;         // optional
};

// ============================================================================
// ListConfig — appearance configuration
// ============================================================================

struct ListConfig {
    std::string indicator      = "\xe2\x96\xb8 ";  // "▸ "
    std::string inactive_prefix = "  ";
    Style active_style   = Style{}.with_bold().with_fg(Color::rgb(97, 175, 239));
    Style inactive_style = Style{}.with_fg(Color::rgb(200, 204, 212));
    Style desc_style     = Style{}.with_fg(Color::rgb(150, 156, 170));
    Style dim_style      = Style{}.with_fg(Color::rgb(92, 99, 112));
    Style filter_style   = Style{}.with_fg(Color::rgb(229, 192, 123));
    int visible_count    = 10;
    bool filterable      = false;
};

// ============================================================================
// List — scrollable selectable list widget
// ============================================================================

class List {
    std::vector<ListItem> items_;
    Signal<int> cursor_{0};
    FocusNode focus_;
    ListConfig cfg_;

    bool filtering_ = false;
    std::string filter_;

    std::move_only_function<void(int, std::string_view)> on_select_;

    // Cached list of visible (filtered) indices into items_
    [[nodiscard]] std::vector<int> visible_indices() const {
        std::vector<int> out;
        out.reserve(items_.size());
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (filter_.empty() || matches_filter(items_[static_cast<size_t>(i)])) {
                out.push_back(i);
            }
        }
        return out;
    }

    [[nodiscard]] bool matches_filter(const ListItem& item) const {
        if (filter_.empty()) return true;
        // Case-insensitive substring match on label
        auto to_lower = [](std::string_view s) {
            std::string r(s);
            for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return r;
        };
        std::string lower_label = to_lower(item.label);
        std::string lower_filter = to_lower(filter_);
        return lower_label.find(lower_filter) != std::string::npos;
    }

public:
    explicit List(std::vector<ListItem> items, ListConfig cfg = {})
        : items_(std::move(items)), cfg_(std::move(cfg)) {}

    // -- Accessors --
    [[nodiscard]] const Signal<int>& cursor()      const { return cursor_; }
    [[nodiscard]] FocusNode& focus_node()                 { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()     const { return focus_; }
    [[nodiscard]] int size()                        const { return static_cast<int>(items_.size()); }
    [[nodiscard]] const std::string& filter()       const { return filter_; }
    [[nodiscard]] bool is_filtering()               const { return filtering_; }

    [[nodiscard]] std::string_view selected_label() const {
        int idx = cursor_();
        if (idx >= 0 && idx < static_cast<int>(items_.size()))
            return items_[static_cast<size_t>(idx)].label;
        return {};
    }

    // -- Callback --
    template <std::invocable<int, std::string_view> F>
    void on_select(F&& fn) { on_select_ = std::forward<F>(fn); }

    // -- Mutation --
    void set_items(std::vector<ListItem> items) {
        items_ = std::move(items);
        filter_.clear();
        filtering_ = false;
        if (cursor_() >= static_cast<int>(items_.size()))
            cursor_.set(std::max(0, static_cast<int>(items_.size()) - 1));
    }

    void set_filter(std::string_view f) {
        filter_ = std::string{f};
        clamp_cursor();
    }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;
        if (items_.empty()) return false;

        // Filter mode input
        if (filtering_) {
            return std::visit(overload{
                [&](SpecialKey sk) -> bool {
                    switch (sk) {
                        case SpecialKey::Enter:
                            filtering_ = false;
                            return true;
                        case SpecialKey::Escape:
                            filtering_ = false;
                            filter_.clear();
                            clamp_cursor();
                            return true;
                        case SpecialKey::Backspace:
                            if (!filter_.empty()) {
                                filter_.pop_back();
                                clamp_cursor();
                            }
                            return true;
                        case SpecialKey::Up:   move_up();   return true;
                        case SpecialKey::Down: move_down(); return true;
                        default: return false;
                    }
                },
                [&](CharKey ck) -> bool {
                    if (ck.codepoint >= 32 && ck.codepoint < 127) {
                        filter_ += static_cast<char>(ck.codepoint);
                        clamp_cursor();
                        return true;
                    }
                    return false;
                },
            }, ev.key);
        }

        // Normal mode
        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Up:   move_up();   return true;
                    case SpecialKey::Down: move_down(); return true;
                    case SpecialKey::Enter:
                        if (on_select_) on_select_(cursor_(), items_[static_cast<size_t>(cursor_())].label);
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'j') { move_down(); return true; }
                if (ck.codepoint == 'k') { move_up();   return true; }
                if (ck.codepoint == '/' && cfg_.filterable) {
                    filtering_ = true;
                    return true;
                }
                return false;
            },
        }, ev.key);
    }

private:
    void clamp_cursor() {
        auto vis = visible_indices();
        if (vis.empty()) return;
        int cur = cursor_();
        // If current cursor is not in visible set, snap to first visible
        bool found = false;
        for (int idx : vis) {
            if (idx == cur) { found = true; break; }
        }
        if (!found) cursor_.set(vis[0]);
    }

    void move_up() {
        auto vis = visible_indices();
        if (vis.empty()) return;
        int cur = cursor_();
        // Find current position in visible list
        int pos = 0;
        for (int i = 0; i < static_cast<int>(vis.size()); ++i) {
            if (vis[static_cast<size_t>(i)] == cur) { pos = i; break; }
        }
        pos = pos > 0 ? pos - 1 : static_cast<int>(vis.size()) - 1;
        cursor_.set(vis[static_cast<size_t>(pos)]);
    }

    void move_down() {
        auto vis = visible_indices();
        if (vis.empty()) return;
        int cur = cursor_();
        int pos = 0;
        for (int i = 0; i < static_cast<int>(vis.size()); ++i) {
            if (vis[static_cast<size_t>(i)] == cur) { pos = i; break; }
        }
        pos = (pos + 1) % static_cast<int>(vis.size());
        cursor_.set(vis[static_cast<size_t>(pos)]);
    }

public:
    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto vis = visible_indices();
        int cur = cursor_();
        bool focused = focus_.focused();

        int total_vis = static_cast<int>(vis.size());
        int start = 0;
        int count = total_vis;

        // Windowed display
        if (cfg_.visible_count > 0 && cfg_.visible_count < total_vis) {
            count = cfg_.visible_count;
            // Find cursor position in visible list
            int cur_pos = 0;
            for (int i = 0; i < total_vis; ++i) {
                if (vis[static_cast<size_t>(i)] == cur) { cur_pos = i; break; }
            }
            start = std::max(0, cur_pos - count / 2);
            if (start + count > total_vis) start = total_vis - count;
        }

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(count) + 2);

        // Filter bar
        if (cfg_.filterable && filtering_) {
            std::string filter_line = "/ " + filter_ + "\xe2\x96\x8e";  // "/ filter▎"
            rows.push_back(Element{TextElement{
                .content = std::move(filter_line),
                .style = cfg_.filter_style,
            }});
        } else if (cfg_.filterable && !filter_.empty()) {
            std::string filter_line = "/ " + filter_;
            rows.push_back(Element{TextElement{
                .content = std::move(filter_line),
                .style = cfg_.dim_style,
            }});
        }

        // Item rows
        for (int i = start; i < start + count; ++i) {
            int item_idx = vis[static_cast<size_t>(i)];
            const auto& item = items_[static_cast<size_t>(item_idx)];
            bool active = (item_idx == cur);

            std::string line;
            std::vector<StyledRun> runs;

            // Prefix: indicator or space
            std::string prefix = active ? cfg_.indicator : cfg_.inactive_prefix;
            size_t prefix_start = 0;
            line += prefix;

            // Icon
            if (!item.icon.empty()) {
                size_t icon_start = line.size();
                line += item.icon;
                line += ' ';
                Style icon_s = active
                    ? (focused ? cfg_.active_style : Style{}.with_bold())
                    : cfg_.inactive_style;
                runs.push_back(StyledRun{icon_start, item.icon.size() + 1, icon_s});
            }

            // Label
            size_t label_start = line.size();
            line += item.label;
            Style label_s = active
                ? (focused ? cfg_.active_style : Style{}.with_bold())
                : cfg_.inactive_style;

            // Prefix run
            runs.insert(runs.begin(), StyledRun{prefix_start, prefix.size(), label_s});
            runs.push_back(StyledRun{label_start, item.label.size(), label_s});

            // Description
            if (!item.description.empty()) {
                line += "  ";
                size_t desc_start = line.size();
                line += item.description;
                runs.push_back(StyledRun{desc_start, item.description.size(), cfg_.desc_style});
            }

            rows.push_back(Element{TextElement{
                .content = std::move(line),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Status line
        std::string status;
        if (!filter_.empty()) {
            status = std::to_string(total_vis) + "/" + std::to_string(items_.size()) + " filtered";
        } else {
            status = std::to_string(items_.size()) + " items";
        }
        rows.push_back(Element{TextElement{
            .content = std::move(status),
            .style = cfg_.dim_style,
        }});

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
