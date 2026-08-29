#pragma once
// maya::widget::EmptyState — empty-panel placeholder (background-free)
//
// The centered "nothing here yet" message panels show when empty: a large dim
// glyph, a title, a hint line, and optional "key — action" call-to-actions.
//
// Usage:
//   EmptyState e;
//   e.glyph("\uf07b").title("No folder open")
//    .hint("Open a folder to start editing")
//    .action("Ctrl+O", "Open Folder").action("Ctrl+N", "New File");
//   Element ui = e | dsl::grow();

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct EmptyStateTheme {
    Color glyph  = Color::hex(0x45475A);
    Color title  = Color::hex(0xBAC2DE);
    Color hint   = Color::hex(0x585B70);
    Color key    = Color::hex(0xF9E2AF);
    Color action = Color::hex(0x9399B2);
};

struct EmptyState {
    std::string glyph_;
    std::string title_;
    std::string hint_;
    std::vector<std::pair<std::string, std::string>> actions_;
    EmptyStateTheme theme;

    EmptyState& glyph(std::string g) { glyph_ = std::move(g); return *this; }
    EmptyState& title(std::string t) { title_ = std::move(t); return *this; }
    EmptyState& hint(std::string h)  { hint_ = std::move(h); return *this; }
    EmptyState& action(std::string key, std::string label) {
        actions_.push_back({std::move(key), std::move(label)}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        auto line = [](std::string t, Style st) {
            return Element{TextElement{ .content = std::move(t), .style = st,
                                        .wrap = TextWrap::NoWrap }};
        };
        if (!glyph_.empty()) rows.push_back(line(glyph_, Style{}.with_fg(theme.glyph)));
        if (!title_.empty()) rows.push_back(line(title_, Style{}.with_fg(theme.title).with_bold()));
        if (!hint_.empty())  rows.push_back(line(hint_, Style{}.with_fg(theme.hint)));
        for (auto& [k, a] : actions_) {
            std::string s; std::vector<StyledRun> r;
            r.push_back({0, k.size(), Style{}.with_fg(theme.key).with_bold()}); s = k;
            std::string tail = "  " + a;
            r.push_back({s.size(), tail.size(), Style{}.with_fg(theme.action)}); s += tail;
            rows.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                                .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }
        return (dsl::v(std::move(rows)) | dsl::gap(1) | dsl::align(Align::Center)
                | dsl::justify(Justify::Center)).build();
    }
};

} // namespace maya
