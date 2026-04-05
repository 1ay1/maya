#include "maya/components/read_card.hpp"

namespace maya::components {

int ReadCard::auto_count() const {
    if (props_.line_count > 0) return props_.line_count;
    if (props_.content.empty()) return 0;
    int n = 1;
    for (char c : props_.content) if (c == '\n') n++;
    return n;
}

void ReadCard::update(const Event& ev) {
    if (key(ev, SpecialKey::Tab)) { toggle_collapsed(); return; }
}

Element ReadCard::render(int frame) const {
    using namespace maya::dsl;
    auto& p = palette();
    Color sc = status_color(props_.status);

    std::string icon;
    if (props_.status == TaskStatus::InProgress) {
        icon = spin(frame);
    } else {
        icon = status_icon(props_.status);
    }

    int lc = auto_count();
    std::string summary_str = std::to_string(lc) + " lines";

    std::vector<Element> header;
    header.push_back(text(icon, Style{}.with_bold().with_fg(sc)));
    header.push_back(text(" ", Style{}));
    header.push_back(text("[Read]", Style{}.with_fg(p.muted)));
    header.push_back(text(" ", Style{}));
    header.push_back(text(props_.file_path, Style{}.with_fg(p.text)));
    header.push_back(text("  ", Style{}));
    header.push_back(text(summary_str, Style{}.with_fg(p.dim)));

    if (!props_.content.empty()) {
        header.push_back(Element(space));
        header.push_back(text(collapsed_ ? "▸" : "▾", Style{}.with_fg(p.dim)));
    }

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    if (!collapsed_ && !props_.content.empty()) {
        rows.push_back(CodeBlock({
            .code = props_.content,
            .language = props_.language,
        }));
    }

    auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(sc)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
