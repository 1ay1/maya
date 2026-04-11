#pragma once
// maya::widget::inline_diff — Word-level inline diff
//
// Two-line diff display with word-level LCS coloring.
// Deleted words are red with strikethrough, added words green with bold.
//
//   InlineDiff diff("const SESSION_TIMEOUT = 3600;",
//                    "const TOKEN_EXPIRY = '1h';");
//   diff.set_label("auth.ts");
//   auto ui = diff.build();

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct InlineDiffConfig {
    Color add_fg      = Color::rgb(152, 195, 121);
    Color del_fg      = Color::rgb(224, 108, 117);
    Color add_bg      = Color::rgb(30, 60, 35);    // word-level highlight
    Color del_bg      = Color::rgb(60, 30, 30);    // word-level highlight
    Color add_line_bg = Color::rgb(18, 35, 22);    // subtle whole-line tint
    Color del_line_bg = Color::rgb(35, 18, 18);    // subtle whole-line tint
    Color same_fg     = Color::rgb(200, 204, 212);
    bool  show_header = true;
};

class InlineDiff {
    std::string before_;
    std::string after_;
    std::string label_;
    InlineDiffConfig cfg_;

    enum class Tag { Same, Del, Add };
    struct Tagged { Tag tag; std::string word; };

    static std::vector<std::string> split_words(const std::string& s) {
        std::vector<std::string> words;
        std::istringstream iss(s);
        std::string w;
        while (iss >> w) words.push_back(std::move(w));
        return words;
    }

public:
    InlineDiff(std::string before, std::string after)
        : before_(std::move(before)), after_(std::move(after)) {}

    InlineDiff(std::string before, std::string after, InlineDiffConfig cfg)
        : before_(std::move(before)), after_(std::move(after)), cfg_(std::move(cfg)) {}

    void set_before(std::string s) { before_ = std::move(s); }
    void set_after(std::string s) { after_ = std::move(s); }
    void set_label(std::string s) { label_ = std::move(s); }
    void set_config(InlineDiffConfig c) { cfg_ = std::move(c); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto words_a = split_words(before_);
        auto words_b = split_words(after_);
        int n = static_cast<int>(words_a.size());
        int m = static_cast<int>(words_b.size());

        // LCS DP
        std::vector<std::vector<int>> dp(
            static_cast<size_t>(n + 1),
            std::vector<int>(static_cast<size_t>(m + 1), 0));
        for (int i = 1; i <= n; ++i)
            for (int j = 1; j <= m; ++j)
                dp[static_cast<size_t>(i)][static_cast<size_t>(j)] =
                    (words_a[static_cast<size_t>(i - 1)] == words_b[static_cast<size_t>(j - 1)])
                        ? dp[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)] + 1
                        : std::max(dp[static_cast<size_t>(i - 1)][static_cast<size_t>(j)],
                                   dp[static_cast<size_t>(i)][static_cast<size_t>(j - 1)]);

        // Backtrack
        std::vector<Tagged> before_tags, after_tags;
        {
            int i = n, j = m;
            std::vector<Tagged> b_rev, a_rev;
            while (i > 0 && j > 0) {
                if (words_a[static_cast<size_t>(i - 1)] == words_b[static_cast<size_t>(j - 1)]) {
                    b_rev.push_back({Tag::Same, words_a[static_cast<size_t>(i - 1)]});
                    a_rev.push_back({Tag::Same, words_b[static_cast<size_t>(j - 1)]});
                    --i; --j;
                } else if (dp[static_cast<size_t>(i - 1)][static_cast<size_t>(j)] >=
                           dp[static_cast<size_t>(i)][static_cast<size_t>(j - 1)]) {
                    b_rev.push_back({Tag::Del, words_a[static_cast<size_t>(i - 1)]});
                    --i;
                } else {
                    a_rev.push_back({Tag::Add, words_b[static_cast<size_t>(j - 1)]});
                    --j;
                }
            }
            while (i > 0) b_rev.push_back({Tag::Del, words_a[static_cast<size_t>(--i)]});
            while (j > 0) a_rev.push_back({Tag::Add, words_b[static_cast<size_t>(--j)]});

            before_tags.assign(b_rev.rbegin(), b_rev.rend());
            after_tags.assign(a_rev.rbegin(), a_rev.rend());
        }

        // Build a styled line from tagged words with full-line background
        auto render_line = [&](const std::vector<Tagged>& tags,
                               const std::string& gutter, Color gutter_color,
                               Color line_bg) -> Element {
            std::vector<Element> parts;

            // Gutter indicator (with line bg)
            parts.push_back(text(gutter, Style{}.with_fg(gutter_color).with_bg(line_bg).with_bold()));
            parts.push_back(text(" ", Style{}.with_bg(line_bg)));

            for (size_t i = 0; i < tags.size(); ++i) {
                if (i > 0) parts.push_back(text(" ", Style{}.with_bg(line_bg)));

                Style s;
                switch (tags[i].tag) {
                    case Tag::Del:
                        s = Style{}.with_fg(cfg_.del_fg).with_bg(cfg_.del_bg).with_strikethrough();
                        break;
                    case Tag::Add:
                        s = Style{}.with_fg(cfg_.add_fg).with_bg(cfg_.add_bg).with_bold();
                        break;
                    case Tag::Same:
                        // Unchanged words get the subtle line-level bg
                        s = Style{}.with_fg(cfg_.same_fg).with_bg(line_bg);
                        break;
                }
                parts.push_back(text(tags[i].word, s));
            }

            return (h(std::move(parts)) | bgc(line_bg)).build();
        };

        std::vector<Element> rows;

        // File header
        if (cfg_.show_header && !label_.empty()) {
            rows.push_back(h(
                text("\xe2\x94\x80\xe2\x94\x80 ", Style{}.with_fg(Color::rgb(62, 68, 81))), // ──
                text(label_, Style{}.with_fg(Color::rgb(200, 204, 212)).with_bold()),
                text(" \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",
                     Style{}.with_fg(Color::rgb(62, 68, 81))) // ─────
            ).build());
        }

        rows.push_back(render_line(before_tags, "\xe2\x88\x92", cfg_.del_fg, cfg_.del_line_bg)); // −
        rows.push_back(render_line(after_tags,  "+", cfg_.add_fg, cfg_.add_line_bg));

        return v(std::move(rows)).build();
    }
};

} // namespace maya
