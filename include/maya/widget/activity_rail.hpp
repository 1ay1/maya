#pragma once
// maya::widget::ActivityBar — vertical icon rail (background-free)
//
// The slim left-most rail of an IDE: stacked action icons (files, search,
// source control, run, extensions…) with an accent bar + bright glyph on the
// active item, small count badges, and a set of items pinned to the bottom
// (settings, account). Fixed width; fills the available height.
//
// Usage:
//   ActivityBar a;
//   a.item("\uf07b").item("\uf002").item("\uf1d3", /*badge=*/3).item("\uf188")
//    .bottom("\uf013").bottom("\uf007")
//    .active(0);
//   Element ui = a;   // give it height via the surrounding layout

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct ActivityBarTheme {
    Color active   = Color::hex(0xF5F5F7); // active glyph
    Color idle     = Color::hex(0x6C7086); // inactive glyph
    Color accent   = Color::hex(0x89B4FA); // active left bar
    Color badge     = Color::hex(0xF38BA8); // count badge
};

class ActivityBar {
public:
    ActivityBar& item(std::string glyph, int badge = 0) {
        top_.push_back({std::move(glyph), badge}); return *this;
    }
    ActivityBar& bottom(std::string glyph, int badge = 0) {
        bottom_.push_back({std::move(glyph), badge}); return *this;
    }
    ActivityBar& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> col;
        int idx = 0;
        for (const auto& it : top_) col.push_back(row(it, idx++ == active_));
        col.push_back(dsl::spacer());
        for (const auto& it : bottom_) col.push_back(row(it, false));
        return (dsl::v(std::move(col)) | dsl::width(4)).build();
    }

private:
    struct Item { std::string glyph; int badge; };
    std::vector<Item> top_, bottom_;
    int               active_ = -1;
    ActivityBarTheme  theme;

    Element row(const Item& it, bool on) const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
        };
        put(on ? "\xe2\x96\x8e " : "  ", Style{}.with_fg(theme.accent)); // ▎
        put(it.glyph, Style{}.with_fg(on ? theme.active : theme.idle));
        if (it.badge > 0)
            put(std::to_string(it.badge % 10), Style{}.with_fg(theme.badge).with_bold());
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
