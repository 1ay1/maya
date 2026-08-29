#pragma once
// maya::widget::GitBlameGutter — inline git blame column (background-free)
//
// The dim author/age column editors show inline to the right of (or beside) the
// code: one row per line with the commit author and a relative time, an accent
// for uncommitted local edits, and a subtle hash. Rendered as a fixed-width
// column so it aligns 1:1 with a CodeView.
//
// Foreground-only.
//
// Usage:
//   GitBlameGutter g{{.width = 22}};
//   g.line("Ayush Bhat",   "3 days ago",  "a1c2f3e")
//    .line("Ada Lovelace", "2 years ago", "9f0b12d")
//    .uncommitted();                       // current line, not yet committed
//   Element ui = g;

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct GitBlameTheme {
    Color author      = Color::hex(0x7F849C); // committer name
    Color when        = Color::hex(0x585B70); // relative time
    Color hash        = Color::hex(0x45475A); // short sha
    Color uncommitted = Color::hex(0x89B4FA); // local, uncommitted line
    Color active      = Color::hex(0x9399B2); // brighten the current line
};

struct GitBlameConfig {
    int           width    = 22;
    bool          show_hash = true;
    GitBlameTheme theme    = {};
};

class GitBlameGutter {
public:
    GitBlameGutter() = default;
    explicit GitBlameGutter(GitBlameConfig cfg) : cfg_(cfg) {}

    GitBlameGutter& line(std::string author, std::string when, std::string hash = {}) {
        lines_.push_back({std::move(author), std::move(when), std::move(hash), false});
        return *this;
    }
    GitBlameGutter& uncommitted() {
        lines_.push_back({"You", "Uncommitted", "", true});
        return *this;
    }
    GitBlameGutter& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(lines_.size());
        for (size_t i = 0; i < lines_.size(); ++i)
            rows.push_back(row(lines_[i], static_cast<int>(i) == active_));
        return dsl::v(std::move(rows)).build();
    }

private:
    struct Blame {
        std::string author, when, hash;
        bool        uncommitted = false;
    };

    GitBlameConfig     cfg_{};
    std::vector<Blame> lines_;
    int                active_ = -1;

    Element row(const Blame& b, bool on) const {
        const auto& th = cfg_.theme;
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return;
            r.push_back({s.size(), t.size(), st});
            s += t;
        };

        if (b.uncommitted) {
            put("\xe2\x97\x8f ", Style{}.with_fg(th.uncommitted));     // ● local
            put("You, uncommitted", Style{}.with_fg(th.uncommitted).with_italic());
            return pad(std::move(s), std::move(r));
        }

        const Color acol = on ? th.active : th.author;
        // "author · when" then a right-aligned short hash within cfg_.width
        std::string head = b.author + "  " + b.when;
        int budget = cfg_.width;
        if (cfg_.show_hash && !b.hash.empty())
            budget -= static_cast<int>(b.hash.size()) + 1;
        head = clip(head, std::max(0, budget));

        // split colouring: author bright, when dim
        size_t sep = head.find("  ");
        if (sep == std::string::npos) {
            put(head, Style{}.with_fg(acol));
        } else {
            put(head.substr(0, sep), Style{}.with_fg(acol));
            put(head.substr(sep), Style{}.with_fg(th.when));
        }
        if (cfg_.show_hash && !b.hash.empty()) {
            put(" ", Style{});
            put(b.hash, Style{}.with_fg(th.hash));
        }
        return pad(std::move(s), std::move(r));
    }

    // Pad/measure the row to a fixed width so blame aligns to the code.
    Element pad(std::string s, std::vector<StyledRun> r) const {
        return Element{ComponentElement{
            .render = [s = std::move(s), r = std::move(r), w = cfg_.width]
                      (int width, int) -> Element {
                std::string content = s;
                std::vector<StyledRun> runs = r;
                int target = std::min(w, width > 0 ? width : w);
                int used = string_width(content);
                if (used < target) {
                    runs.push_back({content.size(),
                                    static_cast<size_t>(target - used), Style{}});
                    content.append(static_cast<size_t>(target - used), ' ');
                }
                return Element{TextElement{ .content = std::move(content),
                                            .style = Style{}, .wrap = TextWrap::NoWrap,
                                            .runs = std::move(runs) }};
            },
            .measure = [w = cfg_.width](int) -> Size {
                return Size{Columns(w), Rows(1)};
            },
        }};
    }

    static std::string clip(std::string s, int max_cols) {
        if (string_width(s) <= max_cols) return s;
        if (max_cols <= 1) return "\xe2\x80\xa6"; // …
        return maya::detail::truncate_end(s, std::max(1, max_cols));
    }
};

} // namespace maya
