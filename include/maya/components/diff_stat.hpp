#pragma once
// maya::components::DiffStat — "+N / -M" colored badge
//
//   DiffStat({.added = 42, .removed = 7})
//   DiffStat({.added = 0, .removed = 15})

#include "core.hpp"

namespace maya::components {

struct DiffStatProps {
    int added   = 0;
    int removed = 0;
};

Element DiffStat(DiffStatProps props);

} // namespace maya::components
