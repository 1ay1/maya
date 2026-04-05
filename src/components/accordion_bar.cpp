#include "maya/components/accordion_bar.hpp"

namespace maya::components {

void AccordionBar::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle(); return; }
}

} // namespace maya::components
