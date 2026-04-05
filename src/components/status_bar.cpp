#include "maya/components/status_bar.hpp"

namespace maya::components {

Element StatusBar(StatusBarProps props) {
    using namespace maya::dsl;

    if (!props.sections.empty()) {
        // Section-based mode
        std::vector<Element> parts;
        for (int i = 0; i < static_cast<int>(props.sections.size()); ++i) {
            auto& sec = props.sections[i];
            Style s = Style{}.with_fg(sec.color).with_bg(props.bg);
            if (sec.bold) s = s.with_bold();

            if (i > 0) {
                parts.push_back(text(" │ ",
                                     Style{}.with_fg(props.separator).with_bg(props.bg)));
            }
            parts.push_back(text(" " + sec.content + " ", s));
        }

        return hstack().bg(props.bg)(std::move(parts));
    }

    // Simple left/center/right mode
    std::vector<Element> parts;

    // Left
    parts.push_back(text(" " + props.left + " ",
                          Style{}.with_bold().with_fg(palette().primary).with_bg(props.bg)));

    // Spacer
    parts.push_back(hstack().grow().bg(props.bg));

    // Center
    if (!props.center.empty()) {
        parts.push_back(text(props.center,
                              Style{}.with_fg(palette().muted).with_bg(props.bg)));
        parts.push_back(hstack().grow().bg(props.bg));
    }

    // Right
    parts.push_back(text(" " + props.right + " ",
                          Style{}.with_fg(palette().dim).with_bg(props.bg)));

    return hstack().bg(props.bg)(std::move(parts));
}

} // namespace maya::components
