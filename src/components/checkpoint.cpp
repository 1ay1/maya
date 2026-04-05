#include "maya/components/checkpoint.hpp"
#include "maya/components/diff_stat.hpp"

namespace maya::components {

void Checkpoint::update(const Event& ev) {
    if (key(ev, 'r')) {
        if (confirmed_) {
            restored_ = true;
            confirmed_ = false;
        } else {
            confirmed_ = true;
        }
        return;
    }
    if (confirmed_ && key(ev, SpecialKey::Escape)) {
        confirmed_ = false;
        return;
    }
}

Element Checkpoint::render() const {
    using namespace maya::dsl;
    auto& p = palette();

    std::vector<Element> header;
    header.push_back(text(restored_ ? "↩" : "⚑", Style{}.with_fg(p.warning).with_bold()));
    header.push_back(text(" ", Style{}));
    header.push_back(text(props_.label, Style{}.with_fg(p.text).with_bold()));

    if (!props_.timestamp.empty()) {
        header.push_back(text("  ", Style{}));
        header.push_back(text(props_.timestamp, Style{}.with_fg(p.dim)));
    }

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    // File count + diff stats
    std::vector<Element> stats;
    if (props_.file_count > 0) {
        stats.push_back(text(std::to_string(props_.file_count) + " files",
                              Style{}.with_fg(p.muted)));
    }
    if (props_.added > 0 || props_.removed > 0) {
        stats.push_back(DiffStat({.added = props_.added, .removed = props_.removed}));
    }
    if (!stats.empty()) {
        rows.push_back(hstack().gap(2)(std::move(stats)));
    }

    if (confirmed_ && !restored_) {
        rows.push_back(text("Press 'r' again to restore, Esc to cancel",
                             Style{}.with_fg(p.warning)));
    }
    if (restored_) {
        rows.push_back(text("Restored to checkpoint", Style{}.with_fg(p.success)));
    }

    auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(p.warning)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
