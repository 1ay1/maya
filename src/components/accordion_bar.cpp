#include "maya/components/accordion_bar.hpp"

namespace maya::components {

void AccordionBar::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle(); return; }
}

Element AccordionBar::render() const {
    using namespace maya::dsl;
    auto& p = palette();

    if (props_.files.empty()) return text("");

    int total_add = 0, total_del = 0;
    for (auto& f : props_.files) {
        total_add += f.added;
        total_del += f.removed;
    }

    std::string title = "File Changes (" + std::to_string(props_.files.size()) + " files)";

    std::vector<Element> header;
    header.push_back(text(expanded_ ? "▾ " : "▸ ", Style{}.with_fg(p.dim)));
    header.push_back(text(title, Style{}.with_fg(p.text).with_bold()));
    header.push_back(text("  ", Style{}));
    header.push_back(DiffStat({.added = total_add, .removed = total_del}));

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    if (expanded_) {
        for (auto& f : props_.files) {
            std::string label = f.path;
            if (f.is_new) label += " (new)";
            if (f.is_deleted) label += " (deleted)";
            rows.push_back(hstack().gap(1)(
                text("  " + label, Style{}.with_fg(p.text)),
                DiffStat({.added = f.added, .removed = f.removed})
            ));
        }
    }

    return vstack()
        .border(BorderStyle::Round)
        .border_color(p.border)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
