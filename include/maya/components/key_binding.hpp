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

Element KeyBinding(KeyBindingProps props);

// Convenience: render a row of keybindings
Element KeyBindings(std::vector<KeyBindingProps> bindings);

} // namespace maya::components
