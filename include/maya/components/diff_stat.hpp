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

inline Element DiffStat(DiffStatProps props) {
    using namespace maya::dsl;

    std::vector<Element> parts;

    if (props.added > 0) {
        parts.push_back(text("+" + std::to_string(props.added),
                             Style{}.with_bold().with_fg(palette().diff_add)));
    }

    if (props.added > 0 && props.removed > 0) {
        parts.push_back(text(" / ", Style{}.with_fg(palette().muted)));
    }

    if (props.removed > 0) {
        parts.push_back(text("-" + std::to_string(props.removed),
                             Style{}.with_bold().with_fg(palette().diff_del)));
    }

    if (parts.empty()) {
        return text("±0", Style{}.with_fg(palette().dim));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
