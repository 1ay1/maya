#include "maya/components/key_binding.hpp"

namespace maya::components {

Element KeyBinding(KeyBindingProps props) {
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

Element KeyBindings(std::vector<KeyBindingProps> bindings) {
    using namespace maya::dsl;

    std::vector<Element> parts;
    for (int i = 0; i < static_cast<int>(bindings.size()); ++i) {
        if (i > 0) parts.push_back(text("  ", Style{}));
        parts.push_back(KeyBinding(std::move(bindings[i])));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
