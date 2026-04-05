#include "maya/components/command_card.hpp"

namespace maya::components {

void CommandCard::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle_collapsed(); return; }
}

} // namespace maya::components
