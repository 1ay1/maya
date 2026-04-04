#pragma once
// maya::components::KeyBinding — Keyboard shortcut display
//
//   KeyBinding({.keys = "Ctrl+C"})
//   KeyBinding({.keys = "Enter", .label = "Send"})
//   KeyBinding({.keys = "q", .label = "quit"})

#include "core.hpp"

namespace maya::components {

struct KeyBindingProps {
    std::string keys;
    std::string label = "";
    Color       key_color   = palette().muted;
    Color       label_color = palette().dim;
};

inline Element KeyBinding(KeyBindingProps props) {
    using namespace maya::dsl;

    std::vector<Element> parts;

    // Key badge
    parts.push_back(text("[" + props.keys + "]",
                          Style{}.with_fg(props.key_color)));

    if (!props.label.empty()) {
        parts.push_back(text(" " + props.label,
                              Style{}.with_fg(props.label_color)));
    }

    return hstack()(std::move(parts));
}

// Convenience: render a row of keybindings
inline Element KeyBindings(std::vector<KeyBindingProps> bindings) {
    using namespace maya::dsl;

    std::vector<Element> parts;
    for (int i = 0; i < static_cast<int>(bindings.size()); ++i) {
        if (i > 0) parts.push_back(text("  ", Style{}));
        parts.push_back(KeyBinding(std::move(bindings[i])));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
