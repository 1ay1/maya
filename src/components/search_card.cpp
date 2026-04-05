#include "maya/components/search_card.hpp"

namespace maya::components {

void SearchCard::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle_collapsed(); return; }
}

} // namespace maya::components
