#pragma once
// maya::components::InlineDiff — Word-level inline diff rendering
//
//   InlineDiff({.before = "const SESSION_TIMEOUT = 3600;",
//               .after  = "const TOKEN_EXPIRY = '1h';"})
//
// Renders two lines ("before:" / "after:") with word-level coloring:
// - Deleted words in red with strikethrough
// - Added words in green with bold
// - Unchanged words in normal text color
// Uses LCS (longest common subsequence) on whitespace-split tokens.

#include "core.hpp"

#include <algorithm>
#include <sstream>

namespace maya::components {

struct InlineDiffProps {
    std::string before;
    std::string after;
    std::string label       = "";
    Color       add_fg      = {};
    Color       del_fg      = {};
    Color       add_bg      = Color::rgb(20, 50, 30);
    Color       del_bg      = Color::rgb(50, 20, 20);
    Color       same_fg     = {};
    bool        show_header = true;
};

inline Element InlineDiff(InlineDiffProps props) {
    using namespace maya::dsl;

    auto& p = palette();
    Color c_add  = props.add_fg  != Color{} ? props.add_fg  : p.diff_add;
    Color c_del  = props.del_fg  != Color{} ? props.del_fg  : p.diff_del;
    Color c_same = props.same_fg != Color{} ? props.same_fg : p.text;

    // Split string into words by whitespace
    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> words;
        std::istringstream iss(s);
        std::string w;
        while (iss >> w) words.push_back(std::move(w));
        return words;
    };

    auto words_a = split(props.before);
    auto words_b = split(props.after);
    int n = static_cast<int>(words_a.size());
    int m = static_cast<int>(words_b.size());

    // LCS DP table — O(n*m)
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            dp[i][j] = (words_a[i - 1] == words_b[j - 1])
                            ? dp[i - 1][j - 1] + 1
                            : std::max(dp[i - 1][j], dp[i][j - 1]);

    // Backtrack to classify each word
    enum class Tag { Same, Del, Add };
    struct Tagged { Tag tag; std::string word; };
    std::vector<Tagged> before_tags, after_tags;

    {
        int i = n, j = m;
        std::vector<Tagged> b_rev, a_rev;
        while (i > 0 && j > 0) {
            if (words_a[i - 1] == words_b[j - 1]) {
                b_rev.push_back({Tag::Same, words_a[i - 1]});
                a_rev.push_back({Tag::Same, words_b[j - 1]});
                --i; --j;
            } else if (dp[i - 1][j] >= dp[i][j - 1]) {
                b_rev.push_back({Tag::Del, words_a[i - 1]});
                --i;
            } else {
                a_rev.push_back({Tag::Add, words_b[j - 1]});
                --j;
            }
        }
        while (i > 0) { b_rev.push_back({Tag::Del, words_a[--i]}); }
        while (j > 0) { a_rev.push_back({Tag::Add, words_b[--j]}); }

        before_tags.assign(b_rev.rbegin(), b_rev.rend());
        after_tags.assign(a_rev.rbegin(), a_rev.rend());
    }

    // Build styled word spans for a tagged list
    auto render_line = [&](const std::vector<Tagged>& tags) {
        std::vector<Element> spans;
        for (auto& t : tags) {
            if (!spans.empty())
                spans.push_back(text(" ", Style{}));

            Style s;
            switch (t.tag) {
                case Tag::Del:
                    s = Style{}.with_fg(c_del).with_bg(props.del_bg).with_strikethrough();
                    break;
                case Tag::Add:
                    s = Style{}.with_fg(c_add).with_bg(props.add_bg).with_bold();
                    break;
                case Tag::Same:
                    s = Style{}.with_fg(c_same);
                    break;
            }
            spans.push_back(text(t.word, s));
        }
        return spans;
    };

    Style dim_style = Style{}.with_dim().with_fg(p.muted);

    std::vector<Element> rows;

    // Optional file header
    if (props.show_header && !props.label.empty()) {
        rows.push_back(text("📄 " + props.label,
                             Style{}.with_bold().with_fg(p.text)));
    }

    // "before:" line
    {
        std::vector<Element> spans;
        spans.push_back(text("  before: ", dim_style));
        auto word_spans = render_line(before_tags);
        spans.insert(spans.end(),
                     std::make_move_iterator(word_spans.begin()),
                     std::make_move_iterator(word_spans.end()));
        rows.push_back(hstack()(std::move(spans)));
    }

    // "after:" line
    {
        std::vector<Element> spans;
        spans.push_back(text("  after:  ", dim_style));
        auto word_spans = render_line(after_tags);
        spans.insert(spans.end(),
                     std::make_move_iterator(word_spans.begin()),
                     std::make_move_iterator(word_spans.end()));
        rows.push_back(hstack()(std::move(spans)));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
