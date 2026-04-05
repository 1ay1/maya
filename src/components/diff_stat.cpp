#include "maya/components/diff_stat.hpp"

namespace maya::components {

Element DiffStat(DiffStatProps props) {
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
