#pragma once
// maya::widget::CommitGraph — git commit graph (background-free)
//
// A vertical commit log with branch lanes: coloured lane rails (│ ├ ┤ ╮ ╯),
// a commit node (●) on its lane, the short hash, refs (HEAD / branch / tag),
// and the subject. A compact take on `git log --graph --oneline`.
//
// Usage:
//   CommitGraph g;
//   g.commit(0, "a1c2f3e", "fix rope index", {"HEAD","main"})
//    .commit(0, "9f0b12d", "add concat")
//    .merge (0, 1, "77aa10c", "merge feature");
//   Element ui = g;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct CommitGraphTheme {
    Color hash    = Color::hex(0xE2B341);
    Color subject = Color::hex(0xBAC2DE);
    Color ref     = Color::hex(0xA6E3A1);
    Color head    = Color::hex(0xF9E2AF);
};

class CommitGraph {
public:
    CommitGraph& commit(int lane, std::string hash, std::string subject,
                        std::vector<std::string> refs = {}) {
        rows_.push_back({lane, -1, std::move(hash), std::move(subject), std::move(refs)});
        return *this;
    }
    CommitGraph& merge(int lane, int from, std::string hash, std::string subject) {
        rows_.push_back({lane, from, std::move(hash), std::move(subject), {}});
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int lanes = 1;
        for (const auto& r : rows_) lanes = std::max(lanes, std::max(r.lane, r.merge_from) + 1);
        std::vector<Element> out;
        for (const auto& r : rows_) out.push_back(row(r, lanes));
        return dsl::v(std::move(out)).build();
    }

private:
    struct Row { int lane; int merge_from; std::string hash, subject; std::vector<std::string> refs; };
    std::vector<Row> rows_;
    CommitGraphTheme theme;

    static Color lane_color(int lane) {
        static const uint32_t pal[] = {0x89B4FA, 0xA6E3A1, 0xF9E2AF, 0xF38BA8, 0xCBA6F7, 0x94E2D5};
        uint32_t h = pal[lane % 6];
        return Color::rgb((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
    }

    Element row(const Row& r, int lanes) const {
        std::string s; std::vector<StyledRun> runs;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            runs.push_back({s.size(), t.size(), st}); s += t; };
        // lane rails
        for (int l = 0; l < lanes; ++l) {
            const char* g = "\xe2\x94\x82"; // │
            if (l == r.lane) g = "\xe2\x97\x8f"; // ● node
            else if (r.merge_from >= 0 && l == r.merge_from) g = "\xe2\x94\x9c"; // ├
            put(g, Style{}.with_fg(lane_color(l)));
            put(" ", Style{});
        }
        put(r.hash + " ", Style{}.with_fg(theme.hash));
        for (const auto& ref : r.refs) {
            bool head = (ref == "HEAD");
            put("(" , Style{}.with_fg(head ? theme.head : theme.ref));
            put(ref, Style{}.with_fg(head ? theme.head : theme.ref).with_bold());
            put(") ", Style{}.with_fg(head ? theme.head : theme.ref));
        }
        put(r.subject, Style{}.with_fg(theme.subject));
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
    }
};

} // namespace maya
