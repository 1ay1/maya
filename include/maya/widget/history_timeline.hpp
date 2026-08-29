#pragma once
// maya::widget::HistoryTimeline — file history timeline (background-free)
//
// The Timeline view: a vertical thread of events (commits, saves, edits) with
// connector rails, a per-kind node glyph + colour, a title, and a dim relative
// time. Good for "local history" and per-file git history.
//
// Usage:
//   HistoryTimeline t;
//   t.event(TimelineKind::Commit, "fix rope index", "2h ago", "a1c2f3e")
//    .event(TimelineKind::Save,   "File Saved",     "3h ago")
//    .event(TimelineKind::Edit,   "42 edits",       "3h ago");
//   Element ui = t | dsl::width(46);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class TimelineKind : uint8_t { Commit, Save, Edit, Branch, Tag };

struct HistoryTimelineTheme {
    Color rail   = Color::hex(0x45475A);
    Color title  = Color::hex(0xBAC2DE);
    Color time   = Color::hex(0x585B70);
    Color hash   = Color::hex(0xE2B341);
    Color commit  = Color::hex(0x89B4FA);
    Color save     = Color::hex(0xA6E3A1);
    Color edit      = Color::hex(0x6C7086);
    Color branch     = Color::hex(0xCBA6F7);
};

class HistoryTimeline {
public:
    HistoryTimeline& event(TimelineKind k, std::string title, std::string when,
                           std::string hash = {}) {
        rows_.push_back({k, std::move(title), std::move(when), std::move(hash)});
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i)
            out.push_back(row(rows_[i], i + 1 == rows_.size()));
        return dsl::v(std::move(out)).build();
    }

private:
    struct E { TimelineKind kind; std::string title, when, hash; };
    std::vector<E> rows_;
    HistoryTimelineTheme theme;

    Color kc(TimelineKind k) const {
        switch (k) { case TimelineKind::Commit: return theme.commit;
                     case TimelineKind::Save: return theme.save;
                     case TimelineKind::Branch: return theme.branch;
                     case TimelineKind::Tag: return theme.branch;
                     default: return theme.edit; }
    }
    const char* kg(TimelineKind k) const {
        switch (k) { case TimelineKind::Commit: return "\xef\x86\x97"; //  commit
                     case TimelineKind::Save: return "\xef\x83\x87";   //  save
                     case TimelineKind::Branch: return "\xef\x84\x98"; //  branch
                     case TimelineKind::Tag: return "\xef\x80\xab";    //  tag
                     default: return "\xef\x81\x84"; }                 //  pencil
    }

    Element row(const E& e, bool last) const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        // rail + node
        put("\xe2\x97\x8f", Style{}.with_fg(kc(e.kind)));           // ● node
        put(last ? "  " : "\xe2\x94\x82 ", Style{}.with_fg(theme.rail)); // │ connector
        put(std::string(kg(e.kind)) + " ", Style{}.with_fg(kc(e.kind)));
        put(e.title, Style{}.with_fg(theme.title));
        if (!e.hash.empty()) put(" " + e.hash, Style{}.with_fg(theme.hash));
        if (!e.when.empty()) put("  " + e.when, Style{}.with_fg(theme.time));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
