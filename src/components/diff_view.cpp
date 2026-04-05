#include "maya/components/diff_view.hpp"

namespace maya::components {

Element DiffView(DiffViewProps props) {
    using namespace maya::dsl;

    std::vector<Element> rows;

    // File header
    if (props.show_header && !props.file_path.empty()) {
        // Count additions and deletions
        int added = 0, removed = 0;
        std::string_view sv = props.diff;
        while (!sv.empty()) {
            auto nl = sv.find('\n');
            auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
            if (!line.empty()) {
                if (line[0] == '+' && !line.starts_with("+++")) ++added;
                if (line[0] == '-' && !line.starts_with("---")) ++removed;
            }
            if (nl == std::string_view::npos) break;
            sv = sv.substr(nl + 1);
        }

        std::vector<Element> header;
        header.push_back(text("📄 " + props.file_path,
                               Style{}.with_bold().with_fg(palette().text)));
        if (added > 0 || removed > 0) {
            header.push_back(text("  ", Style{}));
            if (added > 0)
                header.push_back(text("+" + std::to_string(added),
                                       Style{}.with_bold().with_fg(props.add_fg)));
            if (added > 0 && removed > 0)
                header.push_back(text(" / ", Style{}.with_fg(palette().muted)));
            if (removed > 0)
                header.push_back(text("-" + std::to_string(removed),
                                       Style{}.with_bold().with_fg(props.del_fg)));
        }
        rows.push_back(hstack()(std::move(header)));
    }

    // Parse and render diff lines
    std::string_view src = props.diff;
    while (!src.empty()) {
        auto nl = src.find('\n');
        auto line = (nl == std::string_view::npos) ? src : src.substr(0, nl);

        Style line_style;
        if (line.starts_with("@@")) {
            // Hunk header
            line_style = Style{}.with_fg(props.hunk_fg).with_dim();
        } else if (line.starts_with("+++") || line.starts_with("---")) {
            // File header lines
            line_style = Style{}.with_bold().with_fg(palette().muted);
        } else if (!line.empty() && line[0] == '+') {
            line_style = Style{}.with_fg(props.add_fg).with_bg(props.add_bg);
        } else if (!line.empty() && line[0] == '-') {
            line_style = Style{}.with_fg(props.del_fg).with_bg(props.del_bg);
        } else {
            line_style = Style{}.with_fg(props.context_fg);
        }

        rows.push_back(text(std::string(line), line_style));

        if (nl == std::string_view::npos) break;
        src = src.substr(nl + 1);
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
