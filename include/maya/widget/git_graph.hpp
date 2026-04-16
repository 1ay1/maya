#pragma once
// maya::widget::git_graph — Git commit graph with branch lines
//
// Vertical commit graph with colored branch columns, merge/diverge
// connectors, and aligned hash/message/time columns.
//
//   GitGraph graph;
//   graph.add_commit({"a9f3cf1", "Fix auth token expiry", "", "2m ago", 0, false, true});
//   graph.add_commit({"9bf4e21", "Add rate limiting",     "", "5m ago", 1});
//   graph.add_commit({"a1c3d7f", "Merge branch",          "", "8m ago", 0, true});
//   auto ui = graph.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct GitCommit {
    std::string hash;
    std::string message;
    std::string author   = "";
    std::string time     = "";
    int         branch   = 0;
    bool        is_merge = false;
    bool        is_head  = false;
};

class GitGraph {
    std::vector<GitCommit> commits_;
    int  max_branches_ = 4;
    bool show_hash_    = true;
    bool show_author_  = false;
    bool show_time_    = true;

    // Branch palette: terminal-defined named ANSI colors
    static constexpr Color branch_colors_[] = {
        Color::blue(),
        Color::magenta(),
        Color::green(),
        Color::cyan(),
        Color::yellow(),
    };

    [[nodiscard]] Color branch_color(int col) const {
        return branch_colors_[col % 5];
    }

public:
    GitGraph() = default;

    void set_max_branches(int n) { max_branches_ = std::max(1, n); }
    void set_show_hash(bool b) { show_hash_ = b; }
    void set_show_author(bool b) { show_author_ = b; }
    void set_show_time(bool b) { show_time_ = b; }

    void add_commit(GitCommit c) { commits_.push_back(std::move(c)); }
    void clear() { commits_.clear(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (commits_.empty()) return text("");

        auto dim   = Style{}.with_dim();
        auto txt   = Style{};
        auto muted = Style{}.with_dim();

        int max_col = 0;
        for (auto& c : commits_)
            if (c.branch > max_col) max_col = c.branch;
        max_col = std::min(max_col, max_branches_ - 1);

        // Compute message column width for time alignment
        int max_msg_len = 0;
        for (auto& c : commits_)
            max_msg_len = std::max(max_msg_len, static_cast<int>(c.message.size()));
        max_msg_len = std::min(max_msg_len, 50);

        std::vector<Element> rows;

        for (std::size_t ci = 0; ci < commits_.size(); ++ci) {
            auto& commit = commits_[ci];
            int col = std::min(commit.branch, max_col);
            Color cc = branch_color(col);

            std::vector<Element> parts;

            // Branch lines before commit column
            for (int b = 0; b < col; ++b)
                parts.push_back(text("\xe2\x94\x82 ", Style{}.with_fg(branch_color(b)))); // │

            // Commit node
            if (commit.is_head) {
                parts.push_back(text("\xe2\x97\x8f ", Style{}.with_fg(cc).with_bold())); // ●
            } else if (commit.is_merge) {
                parts.push_back(text("\xe2\x97\x8b ", Style{}.with_fg(cc))); // ○
            } else {
                parts.push_back(text("* ", Style{}.with_fg(cc)));
            }

            // Branch lines after commit column
            for (int b = col + 1; b <= max_col; ++b)
                parts.push_back(text("\xe2\x94\x82 ", Style{}.with_fg(branch_color(b)))); // │

            // Hash
            if (show_hash_) {
                auto hash_style = commit.is_head
                    ? Style{}.with_fg(cc).with_bold()
                    : dim;
                parts.push_back(text(commit.hash, hash_style));
                parts.push_back(text("  "));
            }

            // Message
            auto msg_style = commit.is_head ? txt.with_bold() : txt;
            parts.push_back(text(commit.message, msg_style));

            // Time (right-aligned via padding)
            if (show_time_ && !commit.time.empty()) {
                int pad = max_msg_len - static_cast<int>(commit.message.size());
                if (pad < 2) pad = 2;
                parts.push_back(text(std::string(static_cast<size_t>(pad), ' ')));
                parts.push_back(text(commit.time, muted));
            }

            // Author
            if (show_author_ && !commit.author.empty()) {
                parts.push_back(text("  "));
                parts.push_back(text(commit.author, muted));
            }

            rows.push_back(h(std::move(parts)).build());

            // ── Connector between commits ────────────────────��───────
            if (ci + 1 < commits_.size()) {
                auto& next = commits_[ci + 1];
                int next_col = std::min(next.branch, max_col);

                if (next_col != col) {
                    std::vector<Element> conn;

                    int lo = std::min(col, next_col);
                    int hi = std::max(col, next_col);

                    for (int b = 0; b < lo; ++b)
                        conn.push_back(text("\xe2\x94\x82 ", Style{}.with_fg(branch_color(b))));

                    // Continuing line + branch/merge connector
                    conn.push_back(text("\xe2\x94\x82", Style{}.with_fg(branch_color(lo)))); // │

                    if (next_col > col) {
                        // Diverge: ╲
                        conn.push_back(text("\xe2\x95\xb2", Style{}.with_fg(branch_color(next_col))));
                    } else {
                        // Merge: ╱
                        conn.push_back(text("\xe2\x95\xb1", Style{}.with_fg(branch_color(col))));
                    }

                    rows.push_back(h(std::move(conn)).build());
                }
            }
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
