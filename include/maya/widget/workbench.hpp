#pragma once
// maya::widget::Workbench — IDE shell layout composer (background-free)
//
// Arranges the canonical editor regions into the familiar workbench shape,
// with thin dividers between them and per-region show/hide + sizing:
//
//   ┌───────────────────────────────────────────────┐
//   │ title bar (tabs / command)                     │
//   ├──┬───────────┬────────────────────────────────┤
//   │A │ sidebar   │ editor group                    │
//   │c │ (files /  │                                 │
//   │t │  outline) ├────────────────────────────────┤
//   │  │           │ panel (problems / terminal)     │
//   ├──┴───────────┴────────────────────────────────┤
//   │ status bar                                      │
//   └───────────────────────────────────────────────┘
//
// You hand it the Element for each region (built from the other widgets); it
// only owns the geometry. Any region left unset is omitted.
//
// Usage:
//   Workbench w;
//   w.titlebar(tabbar).activity(rail).sidebar(filetree, 30)
//    .editor(splitview).panel(problems, 9).statusbar(status);
//   Element ui = w | dsl::grow();

#include <optional>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct WorkbenchTheme {
    Color divider = Color::hex(0x1E1E2E);
};

class Workbench {
public:
    Workbench& titlebar(Element e)               { title_ = std::move(e); return *this; }
    Workbench& activity(Element e)               { activity_ = std::move(e); return *this; }
    Workbench& sidebar(Element e, int width = 30) { sidebar_ = std::move(e); side_w_ = width; return *this; }
    Workbench& editor(Element e)                 { editor_ = std::move(e); return *this; }
    Workbench& panel(Element e, int height = 10)  { panel_ = std::move(e); panel_h_ = height; return *this; }
    Workbench& statusbar(Element e)              { status_ = std::move(e); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // editor group, optionally split with a bottom panel
        Element center = editor_ ? *editor_ : Element{};
        if (panel_) {
            center = dsl::v(
                (center | dsl::grow()),
                hdivider(),
                (*panel_ | dsl::height(panel_h_))
            ).build();
        } else if (editor_) {
            center = (center | dsl::grow()).build();
        }

        // body row: activity | sidebar | center
        std::vector<Element> body;
        if (activity_) { body.push_back(*activity_); body.push_back(vdivider()); }
        if (sidebar_)  { body.push_back(*sidebar_ | dsl::width(side_w_)); body.push_back(vdivider()); }
        body.push_back(center | dsl::grow());
        Element bodyEl = (dsl::h(std::move(body)) | dsl::grow()).build();

        // full column: title | body | status
        std::vector<Element> col;
        if (title_) col.push_back(*title_); // the title bar owns its own separator
        col.push_back(bodyEl);
        if (status_) { col.push_back(hdivider()); col.push_back(*status_); }

        return (dsl::v(std::move(col)) | dsl::grow()).build();
    }

private:
    std::optional<Element> title_, activity_, sidebar_, editor_, panel_, status_;
    int                    side_w_  = 30;
    int                    panel_h_ = 10;
    WorkbenchTheme         theme;

    Element vdivider() const {
        const Color c = theme.divider;
        return Element{ComponentElement{
            .render = [c](int, int h) -> Element {
                std::vector<Element> rows;
                for (int i = 0; i < std::max(1, h); ++i)
                    rows.push_back(Element{TextElement{
                        .content = "\xe2\x94\x82", .style = Style{}.with_fg(c) }}); // │
                return dsl::v(std::move(rows)).build();
            },
            .measure = [](int) -> Size { return Size{Columns(1), Rows(1)}; },
        }};
    }
    Element hdivider() const {
        const Color c = theme.divider;
        return Element{ComponentElement{
            .render = [c](int w, int) -> Element {
                std::string s;
                for (int i = 0; i < std::max(1, w); ++i) s += "\xe2\x94\x80"; // ─
                return Element{TextElement{ .content = std::move(s),
                                            .style = Style{}.with_fg(c) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
