#pragma once
// maya::widget::ZenLayout — centered max-width container (background-free)
//
// Writing / zen mode: centers its content horizontally and caps it at a maximum
// width, leaving the margins empty (the terminal's own background). Wrap any
// content Element (a CodeView, markdown, an editor) to get a comfortable,
// centered reading column.
//
// Usage:  ZenLayout{}.max_width(80).content(editor);

#include <algorithm>
#include <utility>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct ZenLayout {
    Element content_{TextElement{}};
    int     max_width_ = 80;

    ZenLayout& content(Element e) { content_ = std::move(e); return *this; }
    ZenLayout& max_width(int w) { max_width_ = w; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        Element body = content_;
        int mw = max_width_;
        return Element{ComponentElement{
            .render = [body = std::move(body), mw](int w, int) -> Element {
                const int cw = std::clamp(mw, 1, std::max(1, w));
                const int side = std::max(0, (w - cw) / 2);
                return dsl::h(
                    Element{TextElement{}} | dsl::width(side),
                    body | dsl::width(cw),
                    dsl::spacer()
                ).build();
            },
            .measure = [](int mw2) -> Size { return Size{Columns(mw2), Rows(1)}; },
        }};
    }
};

} // namespace maya
