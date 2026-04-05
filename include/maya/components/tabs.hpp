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
    void set_active(int i);

    void next() { set_active(active_ + 1); }
    void prev() { set_active(active_ - 1); }

    bool update(const Event& ev);

    [[nodiscard]] Element render() const;

    [[nodiscard]] Element render_with(const Children& panels) const;
};

} // namespace maya::components
