#pragma once
// maya::widget::NotificationStack — toast / notification cards (background-free)
//
// A vertical stack of notification cards, each a thin rounded box tinted by
// kind (info / success / warning / error / progress): a leading accent bar +
// glyph, a bold title, an optional dim body, and an optional progress bar.
// Foreground-only.
//
// Usage:
//   NotificationStack n;
//   n.toast(Toast::Success, "Saved", "rope.cpp written (2.1 KB)")
//    .toast(Toast::Progress, "Indexing", "1,204 / 3,900 files", 0.31f)
//    .toast(Toast::Error, "Build failed", "3 errors in editor.cpp");
//   Element ui = n | dsl::width(46);

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct NotificationStack {
    enum Kind : uint8_t { Info, Success, Warning, Error, Progress };
    struct Item { Kind kind; std::string title; std::string body; float progress; };

    // expose kind names for call sites
    static constexpr Kind ToastInfo = Info, ToastSuccess = Success,
                          ToastWarning = Warning, ToastError = Error,
                          ToastProgress = Progress;

    std::vector<Item> items;
    int               bar_width = 28;

    NotificationStack& toast(Kind k, std::string title, std::string body = {},
                             float progress = -1.0f) {
        items.push_back({k, std::move(title), std::move(body), progress});
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> stack;
        for (const auto& it : items) stack.push_back(card(it));
        return dsl::v(std::move(stack)).build();
    }

private:
    Color kind_color(Kind k) const {
        switch (k) {
            case Success:  return Color::hex(0xA6E3A1);
            case Warning:  return Color::hex(0xF9E2AF);
            case Error:    return Color::hex(0xF38BA8);
            case Progress: return Color::hex(0xCBA6F7);
            default:       return Color::hex(0x89B4FA);
        }
    }
    const char* kind_glyph(Kind k) const {
        switch (k) {
            case Success:  return "\xef\x81\x98"; //  check-circle
            case Warning:  return "\xef\x81\xb1"; //  warning
            case Error:    return "\xef\x81\x97"; //  times-circle
            case Progress: return "\xef\x84\x90"; //  spinner-ish
            default:       return "\xef\x81\x9a"; //  info-circle
        }
    }

    Element card(const Item& it) const {
        const Color c = kind_color(it.kind);
        std::vector<Element> rows;

        // title row: ▎ glyph  Title
        {
            std::string s; std::vector<StyledRun> r;
            auto put = [&](std::string_view t, Style st) {
                if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
            };
            put("\xe2\x96\x8e ", Style{}.with_fg(c)); // ▎
            put(std::string(kind_glyph(it.kind)) + "  ", Style{}.with_fg(c));
            put(it.title, Style{}.with_fg(Color::hex(0xE6EDF3)).with_bold());
            rows.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                                .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }

        if (!it.body.empty())
            rows.push_back(Element{TextElement{
                .content = "  " + it.body,
                .style = Style{}.with_fg(Color::hex(0x9399B2)), .wrap = TextWrap::Wrap }});

        if (it.progress >= 0.0f) {
            const int filled = std::clamp(static_cast<int>(it.progress * bar_width + 0.5f),
                                          0, bar_width);
            std::string s = "  "; std::vector<StyledRun> r;
            r.push_back({0, 2, Style{}});
            std::string on, off;
            for (int i = 0; i < filled; ++i) on += "\xe2\x94\x81";        // ━
            for (int i = filled; i < bar_width; ++i) off += "\xe2\x94\x80"; // ─
            r.push_back({s.size(), on.size(), Style{}.with_fg(c)}); s += on;
            r.push_back({s.size(), off.size(), Style{}.with_fg(Color::hex(0x45475A))}); s += off;
            std::string pct = "  " + std::to_string(static_cast<int>(it.progress * 100)) + "%";
            r.push_back({s.size(), pct.size(), Style{}.with_fg(c)}); s += pct;
            rows.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                                .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(c)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(rows)).build());
    }
};

} // namespace maya
