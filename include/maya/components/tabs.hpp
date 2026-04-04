#pragma once
// maya::components::Tabs — Tab bar with active indicator
//
//   Tabs tabs({.labels = {"Chat", "History", "Settings"}});
//
//   // In event handler:
//   tabs.update(ev);  // handles Tab/BackTab, number keys
//
//   // Render just the tab bar:
//   tabs.render()
//
//   // Or render with content panels:
//   tabs.render_with({chat_panel, history_panel, settings_panel})

#include "core.hpp"

namespace maya::components {

struct TabsProps {
    std::vector<std::string> labels = {};
    int  active    = 0;
    Color color    = palette().primary;
};

class Tabs {
    int active_;
    TabsProps props_;

public:
    explicit Tabs(TabsProps props = {})
        : active_(props.active), props_(std::move(props)) {}

    [[nodiscard]] int active() const { return active_; }
    void set_active(int i) {
        int n = static_cast<int>(props_.labels.size());
        if (n > 0) active_ = ((i % n) + n) % n;
    }

    void next() { set_active(active_ + 1); }
    void prev() { set_active(active_ - 1); }

    bool update(const Event& ev) {
        if (key(ev, SpecialKey::Tab)) { next(); return true; }
        if (shift(ev, SpecialKey::Tab)) { prev(); return true; }

        // Number keys 1-9 to select tab
        auto* k = as_key(ev);
        if (k) {
            if (auto* ch = std::get_if<CharKey>(&k->key)) {
                if (ch->codepoint >= '1' && ch->codepoint <= '9') {
                    int idx = static_cast<int>(ch->codepoint - '1');
                    if (idx < static_cast<int>(props_.labels.size())) {
                        active_ = idx;
                        return true;
                    }
                }
            }
        }

        return false;
    }

    [[nodiscard]] Element render() const {
        using namespace maya::dsl;

        std::vector<Element> tab_items;
        for (int i = 0; i < static_cast<int>(props_.labels.size()); ++i) {
            bool is_active = (i == active_);
            Style s;
            if (is_active) {
                s = Style{}.with_bold().with_fg(props_.color);
            } else {
                s = Style{}.with_fg(palette().muted);
            }

            std::string label = " " + props_.labels[i] + " ";
            if (is_active) {
                // Active tab with underline indicator
                std::string underline;
                for (size_t j = 0; j < label.size(); ++j) underline += "─";
                tab_items.push_back(vstack()(
                    text(label, s),
                    text(std::move(underline), Style{}.with_fg(props_.color))
                ));
            } else {
                tab_items.push_back(vstack()(
                    text(label, s),
                    text(std::string(label.size(), ' '), Style{})
                ));
            }
        }

        return hstack()(std::move(tab_items));
    }

    [[nodiscard]] Element render_with(const Children& panels) const {
        using namespace maya::dsl;

        std::vector<Element> rows;
        rows.push_back(render());

        // Show active panel
        if (active_ >= 0 && active_ < static_cast<int>(panels.size())) {
            rows.push_back(panels[active_]);
        }

        return vstack()(std::move(rows));
    }
};

} // namespace maya::components
