#include "maya/components/edit_card.hpp"

namespace maya::components {

void EditCard::parse_hunks() {
    hunks_.clear();
    std::string_view sv = props_.diff;
    std::string current_header;
    std::string current_content;

    while (!sv.empty()) {
        auto nl = sv.find('\n');
        auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);

        if (line.starts_with("@@")) {
            // Save previous hunk
            if (!current_header.empty()) {
                hunks_.push_back({current_header, current_content});
                current_content.clear();
            }
            current_header = std::string(line);
        } else if (!current_header.empty() &&
                   !line.starts_with("---") && !line.starts_with("+++")) {
            if (!current_content.empty()) current_content += '\n';
            current_content += std::string(line);
        }

        if (nl == std::string_view::npos) break;
        sv = sv.substr(nl + 1);
    }
    if (!current_header.empty()) {
        hunks_.push_back({current_header, current_content});
    }
}

void EditCard::count_stats() {
    if (props_.added >= 0 && props_.removed >= 0) return;
    int a = 0, r = 0;
    std::string_view sv = props_.diff;
    while (!sv.empty()) {
        auto nl = sv.find('\n');
        auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
        if (!line.empty()) {
            if (line[0] == '+' && !line.starts_with("+++")) ++a;
            if (line[0] == '-' && !line.starts_with("---")) ++r;
        }
        if (nl == std::string_view::npos) break;
        sv = sv.substr(nl + 1);
    }
    if (props_.added < 0) props_.added = a;
    if (props_.removed < 0) props_.removed = r;
}

bool EditCard::accepted() const {
    for (auto& h : hunks_)
        if (h.decision != EditDecision::Accepted) return false;
    return !hunks_.empty();
}

bool EditCard::rejected() const {
    for (auto& h : hunks_)
        if (h.decision != EditDecision::Rejected) return false;
    return !hunks_.empty();
}

EditDecision EditCard::decision() const {
    if (accepted()) return EditDecision::Accepted;
    if (rejected()) return EditDecision::Rejected;
    return EditDecision::Pending;
}

void EditCard::accept_hunk(int i) {
    if (i >= 0 && i < static_cast<int>(hunks_.size()))
        hunks_[i].decision = EditDecision::Accepted;
}

void EditCard::reject_hunk(int i) {
    if (i >= 0 && i < static_cast<int>(hunks_.size()))
        hunks_[i].decision = EditDecision::Rejected;
}

void EditCard::update(const Event& ev) {
    // Accept all
    if (key(ev, 'a') || key(ev, 'y')) { accept_all(); return; }
    // Reject all
    if (key(ev, 'x') || key(ev, 'n')) { reject_all(); return; }
    // Toggle collapse
    if (key(ev, SpecialKey::Tab)) { toggle_collapsed(); return; }
    // Navigate hunks
    if (key(ev, SpecialKey::Up) || key(ev, 'k')) {
        if (selected_ > 0) --selected_;
        return;
    }
    if (key(ev, SpecialKey::Down) || key(ev, 'j')) {
        if (selected_ < static_cast<int>(hunks_.size()) - 1) ++selected_;
        return;
    }
    // Accept current hunk
    if (key(ev, SpecialKey::Enter)) { accept_hunk(selected_); return; }
    // Reject current hunk
    if (key(ev, SpecialKey::Backspace)) { reject_hunk(selected_); return; }
}

Element EditCard::render(int frame) const {
    using namespace maya::dsl;
    auto& p = palette();
    Color sc = status_color(props_.status);

    // Icon
    std::string icon;
    if (props_.status == TaskStatus::InProgress)
        icon = spin(frame);
    else
        icon = status_icon(props_.status);

    // Header
    std::vector<Element> header;
    header.push_back(text(icon, Style{}.with_bold().with_fg(sc)));
    header.push_back(text(" ", Style{}));
    header.push_back(text("📄 " + props_.file_path,
                           Style{}.with_bold().with_fg(p.text)));
    header.push_back(text("  ", Style{}));

    if (props_.added > 0)
        header.push_back(text("+" + std::to_string(props_.added),
                               Style{}.with_bold().with_fg(p.diff_add)));
    if (props_.added > 0 && props_.removed > 0)
        header.push_back(text(" / ", Style{}.with_fg(p.muted)));
    if (props_.removed > 0)
        header.push_back(text("-" + std::to_string(props_.removed),
                               Style{}.with_bold().with_fg(p.diff_del)));

    header.push_back(Element(space));
    header.push_back(text(collapsed_ ? "▸" : "▾", Style{}.with_fg(p.dim)));

    std::vector<Element> rows;
    rows.push_back(hstack()(std::move(header)));

    // Body
    if (!collapsed_) {
        for (int i = 0; i < static_cast<int>(hunks_.size()); ++i) {
            auto& hunk = hunks_[i];
            bool is_selected = (i == selected_);

            // Hunk header with decision indicator
            std::string dec_icon;
            Color dec_color;
            switch (hunk.decision) {
                case EditDecision::Accepted:
                    dec_icon = "✓"; dec_color = p.success; break;
                case EditDecision::Rejected:
                    dec_icon = "✗"; dec_color = p.error; break;
                case EditDecision::Pending:
                    dec_icon = "○"; dec_color = p.muted; break;
            }

            rows.push_back(
                hstack().gap(1)(
                    text(is_selected ? "▸" : " ", Style{}.with_fg(p.primary)),
                    text(dec_icon, Style{}.with_fg(dec_color)),
                    text(hunk.header, Style{}.with_fg(p.info).with_dim())
                )
            );

            // Diff content
            rows.push_back(DiffView({
                .diff = hunk.content,
                .show_header = false,
            }));
        }

        // Keyhints
        if (props_.show_keyhints) {
            rows.push_back(KeyBindings({
                {.keys = "a", .label = "accept all"},
                {.keys = "x", .label = "reject all"},
                {.keys = "↑↓", .label = "navigate"},
                {.keys = "Enter", .label = "accept"},
                {.keys = "⌫", .label = "reject"},
                {.keys = "Tab", .label = "collapse"},
            }));
        }
    }

    auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(sc)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
