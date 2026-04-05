#include "maya/components/git_graph.hpp"

namespace maya::components {

Element GitGraph(GitGraphProps props) {
    using namespace maya::dsl;

    auto& p = palette();

    // Branch colors: main = primary, then cycle accent, secondary, info, warning
    auto branch_color = [&](int col) -> Color {
        if (col == 0) return p.primary;
        constexpr int n = 4;
        Color cycle[n] = { p.accent, p.secondary, p.info, p.warning };
        return cycle[(col - 1) % n];
    };

    // Track which branch columns are currently active
    int max_col = 0;
    for (auto& c : props.commits)
        if (c.branch > max_col) max_col = c.branch;
    max_col = std::min(max_col, props.max_branches - 1);

    std::vector<Element> rows;

    for (std::size_t ci = 0; ci < props.commits.size(); ++ci) {
        auto& commit = props.commits[ci];
        int col = std::min(commit.branch, max_col);

        // ── Build the graph prefix (branch lines + commit node) ──────────

        std::vector<Element> line_parts;

        // Draw columns before the commit's branch
        for (int b = 0; b < col; ++b) {
            line_parts.push_back(
                text("│ ", Style{}.with_fg(branch_color(b))));
        }

        // Draw the commit node
        const char* node = commit.is_head ? "●" : "*";
        Style node_style = Style{}.with_fg(branch_color(col));
        if (commit.is_head) node_style = node_style.with_bold();
        line_parts.push_back(text(node, node_style));
        line_parts.push_back(text(" ", Style{}));

        // Draw columns after the commit's branch
        for (int b = col + 1; b <= max_col; ++b) {
            line_parts.push_back(
                text("│ ", Style{}.with_fg(branch_color(b))));
        }

        // ── Build the commit info ────────────────────────────────────────

        if (props.show_hash) {
            Style hash_style = Style{}.with_fg(p.dim);
            if (commit.is_head) hash_style = Style{}.with_fg(p.primary).with_bold();
            line_parts.push_back(text(commit.hash, hash_style));
            line_parts.push_back(text("  ", Style{}));
        }

        // Message
        Style msg_style = Style{}.with_fg(p.text);
        if (commit.is_head) msg_style = msg_style.with_bold();
        line_parts.push_back(text(commit.message, msg_style));

        // Author
        if (props.show_author && !commit.author.empty()) {
            line_parts.push_back(text("  ", Style{}));
            line_parts.push_back(
                text(commit.author, Style{}.with_fg(p.muted)));
        }

        // Time
        if (props.show_time && !commit.time.empty()) {
            int pad = 40 - static_cast<int>(commit.message.size());
            if (pad < 2) pad = 2;
            line_parts.push_back(text(std::string(pad, ' '), Style{}));
            line_parts.push_back(
                text(commit.time, Style{}.with_fg(p.muted)));
        }

        rows.push_back(hstack()(std::move(line_parts)));

        // ── Draw merge / branch lines between commits ────────────────────

        // Look ahead: if next commit is on a different branch, draw connector lines
        if (ci + 1 < props.commits.size()) {
            auto& next = props.commits[ci + 1];
            int next_col = std::min(next.branch, max_col);

            if (commit.is_merge && col == 0 && next_col > 0) {
                // Merge from a side branch into main — draw |\  lines
                std::vector<Element> merge_parts;
                merge_parts.push_back(
                    text("│", Style{}.with_fg(branch_color(0))));
                merge_parts.push_back(
                    text("╲", Style{}.with_fg(branch_color(next_col))));
                rows.push_back(hstack()(std::move(merge_parts)));
            } else if (next_col > col) {
                // Branch diverges — draw |\ connector
                std::vector<Element> branch_parts;
                for (int b = 0; b < col; ++b) {
                    branch_parts.push_back(
                        text("│ ", Style{}.with_fg(branch_color(b))));
                }
                branch_parts.push_back(
                    text("│", Style{}.with_fg(branch_color(col))));
                branch_parts.push_back(
                    text("╲", Style{}.with_fg(branch_color(next_col))));
                rows.push_back(hstack()(std::move(branch_parts)));
            } else if (next_col < col) {
                // Branch merges back — draw |/ connector
                std::vector<Element> merge_parts;
                for (int b = 0; b < next_col; ++b) {
                    merge_parts.push_back(
                        text("│ ", Style{}.with_fg(branch_color(b))));
                }
                merge_parts.push_back(
                    text("│", Style{}.with_fg(branch_color(next_col))));
                merge_parts.push_back(
                    text("╱", Style{}.with_fg(branch_color(col))));
                rows.push_back(hstack()(std::move(merge_parts)));
            }
        }
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
