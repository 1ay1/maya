#pragma once
// maya::widget::ContextMenu — right-click / dropdown menu (background-free)
//
// A floating menu: rows of icon + label with a right-aligned shortcut or a
// submenu arrow, separators, disabled (dim) items, and checkmarks. The active
// row gets a soft full-width shade. Thin rounded border.
//
// Usage:
//   ContextMenu m;
//   m.item("Go to Definition", "F12", "\uf1c9")
//    .item("Peek References",  "\u21e7 F12")
//    .separator()
//    .item("Rename Symbol",    "F2")
//    .check("Word Wrap", true)
//    .disabled("Format Selection")
//    .submenu("Refactor")
//    .active(0);
//   Element ui = m | dsl::width(40);

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

struct ContextMenuTheme {
    Color border   = Color::hex(0x313244);
    Color label    = Color::hex(0xCDD6F4);
    Color icon     = Color::hex(0x89B4FA);
    Color shortcut = Color::hex(0x6C7086);
    Color disabled = Color::hex(0x494D64);
    Color check     = Color::hex(0xA6E3A1);
    Color shade      = Color::hex(0x313244);
    Color sep         = Color::hex(0x313244);
};

struct ContextMenu {
    struct Item {
        std::string label;
        std::string shortcut;
        std::string icon;
        bool        separator = false;
        bool        disabled  = false;
        bool        submenu   = false;
        bool        checked   = false;
    };

    std::vector<Item> items;
    int               active_ = -1;
    ContextMenuTheme  theme;

    ContextMenu& item(std::string label, std::string sc = {}, std::string icon = {}) {
        items.push_back({std::move(label), std::move(sc), std::move(icon)}); return *this;
    }
    ContextMenu& disabled(std::string label, std::string sc = {}) {
        Item it{std::move(label), std::move(sc)}; it.disabled = true; items.push_back(std::move(it)); return *this;
    }
    ContextMenu& submenu(std::string label) {
        Item it{std::move(label)}; it.submenu = true; items.push_back(std::move(it)); return *this;
    }
    ContextMenu& check(std::string label, bool on) {
        Item it{std::move(label)}; it.checked = on; items.push_back(std::move(it)); return *this;
    }
    ContextMenu& separator() { Item it; it.separator = true; items.push_back(std::move(it)); return *this; }
    ContextMenu& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        for (size_t i = 0; i < items.size(); ++i)
            rows.push_back(items[i].separator ? sep_row()
                                              : item_row(items[i], static_cast<int>(i) == active_));
        return maya::detail::box()
            .border(BorderStyle::Round).border_color(theme.border)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(rows)).build());
    }

private:
    Element sep_row() const {
        return Element{ComponentElement{
            .render = [c = theme.sep](int w, int) -> Element {
                std::string s; for (int i = 0; i < w; ++i) s += "\xe2\x94\x80"; // ─
                return Element{TextElement{ .content = std::move(s),
                                            .style = Style{}.with_fg(c) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }

    Element item_row(const Item& it, bool on) const {
        const Color shade = on ? theme.shade : Color{};
        auto tint = [on, shade](Style st){ return on ? st.with_bg(shade) : st; };
        const Color lc = it.disabled ? theme.disabled : theme.label;

        std::string left; std::vector<StyledRun> lr;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(), t.size(), tint(st)}); left += t; };

        if (it.checked) put("\xe2\x9c\x93 ", Style{}.with_fg(theme.check)); // ✓
        else            put("  ", Style{});
        if (!it.icon.empty()) put(it.icon + " ", Style{}.with_fg(it.disabled ? theme.disabled : theme.icon));
        put(it.label, Style{}.with_fg(lc));

        std::string right = it.submenu ? "\xef\x84\x85" /* chevron-right */ : it.shortcut;
        Color rcol = it.submenu ? theme.label : theme.shortcut;

        return Element{ComponentElement{
            .render = [left = std::move(left), lr = std::move(lr), right = std::move(right),
                       rcol, on, shade](int w, int) -> Element {
                const Style fill = on ? Style{}.with_bg(shade) : Style{};
                std::string s = left; std::vector<StyledRun> runs = lr;
                int rw = string_width(right);
                int gap = std::max(1, w - string_width(s) - rw);
                runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                s.append(static_cast<size_t>(gap), ' ');
                if (!right.empty()) {
                    Style rs = Style{}.with_fg(rcol);
                    if (on) rs = rs.with_bg(shade);
                    runs.push_back({s.size(), right.size(), rs}); s += right;
                }
                int total = string_width(s);
                if (total < w) { runs.push_back({s.size(), static_cast<size_t>(w - total), fill});
                                 s.append(static_cast<size_t>(w - total), ' '); }
                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
