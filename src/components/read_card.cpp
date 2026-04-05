#include "maya/components/read_card.hpp"

namespace maya::components {

void ReadCard::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle_collapsed(); return; }
}

} // namespace maya::components
