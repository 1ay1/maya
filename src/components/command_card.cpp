#include "maya/components/command_card.hpp"

namespace maya::components {

void CommandCard::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle_collapsed(); return; }
}

Element CommandCard::render(int frame) const {
    using namespace maya::dsl;
    auto& p = palette();
    Color sc = status_color(props_.status);

    std::string icon;
    if (props_.status == TaskStatus::InProgress) {
        icon = spin(frame);
    } else {
        icon = status_icon(props_.status);
    }

    std::vector<Element> header;
    header.push_back(text(icon, Style{}.with_bold().with_fg(sc)));
    header.push_back(text(" ", Style{}));
    header.push_back(text("[Bash]", Style{}.with_fg(p.muted)));
    header.push_back(text(" ", Style{}));
    header.push_back(text(props_.command, Style{}.with_fg(p.text).with_bold()));

    if (props_.exit_code >= 0) {
        header.push_back(text("  ", Style{}));
        Color exit_col = props_.exit_code == 0 ? p.success : p.error;
        header.push_back(text("exit " + std::to_string(props_.exit_code),
                               Style{}.with_fg(exit_col)));
    }

    if (!props_.output.empty()) {
        header.push_back(Element(space));
        header.push_back(text(collapsed_ ? "▸" : "▾", Style{}.with_fg(p.dim)));
    }

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    if (!collapsed_ && !props_.output.empty()) {
        // Show output as dim text, truncated to max_lines
        std::string out = props_.output;
        int lines = 0;
        for (size_t i = 0; i < out.size(); i++) {
            if (out[i] == '\n') lines++;
            if (lines >= props_.max_lines) {
                out = out.substr(0, i) + "\n...";
                break;
            }
        }
        rows.push_back(text(out, Style{}.with_fg(p.muted)));
    }

    auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(sc)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
