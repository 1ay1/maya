#include "maya/element/builder.hpp"

namespace maya {

// ============================================================================
// BoxBuilder property setters
// ============================================================================

auto BoxBuilder::direction(FlexDirection d) -> BoxBuilder& {
    element_.layout.direction = d;
    return *this;
}

auto BoxBuilder::wrap(FlexWrap w) -> BoxBuilder& {
    element_.layout.wrap = w;
    return *this;
}

auto BoxBuilder::padding(int all) -> BoxBuilder& {
    element_.layout.padding = {all, all, all, all};
    return *this;
}

auto BoxBuilder::padding(int v, int h) -> BoxBuilder& {
    element_.layout.padding = {v, h, v, h};
    return *this;
}

auto BoxBuilder::padding(int top, int right, int bottom, int left) -> BoxBuilder& {
    element_.layout.padding = {top, right, bottom, left};
    return *this;
}

auto BoxBuilder::margin(int all) -> BoxBuilder& {
    element_.layout.margin = {all, all, all, all};
    return *this;
}

auto BoxBuilder::margin(int v, int h) -> BoxBuilder& {
    element_.layout.margin = {v, h, v, h};
    return *this;
}

auto BoxBuilder::margin(int top, int right, int bottom, int left) -> BoxBuilder& {
    element_.layout.margin = {top, right, bottom, left};
    return *this;
}

auto BoxBuilder::border(BorderStyle bs) -> BoxBuilder& {
    element_.border.style = bs;
    return *this;
}

auto BoxBuilder::border(BorderStyle bs, Color c) -> BoxBuilder& {
    element_.border.style = bs;
    element_.border.colors = BorderColors::uniform(c);
    return *this;
}

auto BoxBuilder::border_color(Color c) -> BoxBuilder& {
    element_.border.colors = BorderColors::uniform(c);
    return *this;
}

auto BoxBuilder::border_sides(BorderSides sides) -> BoxBuilder& {
    element_.border.sides = sides;
    return *this;
}

auto BoxBuilder::border_text(std::string_view content,
                             BorderTextPos pos) -> BoxBuilder& {
    element_.border.text = BorderText{.content = std::string{content}, .position = pos};
    return *this;
}

auto BoxBuilder::border_text(std::string_view content,
                             BorderTextPos pos,
                             BorderTextAlign align) -> BoxBuilder& {
    element_.border.text = BorderText{
        .content = std::string{content},
        .position = pos,
        .align = align,
    };
    return *this;
}

auto BoxBuilder::grow(float g) -> BoxBuilder& {
    element_.layout.grow = g;
    return *this;
}

auto BoxBuilder::shrink(float s) -> BoxBuilder& {
    element_.layout.shrink = s;
    return *this;
}

auto BoxBuilder::basis(Dimension d) -> BoxBuilder& {
    element_.layout.basis = d;
    return *this;
}

auto BoxBuilder::width(Dimension d) -> BoxBuilder& {
    element_.layout.width = d;
    return *this;
}

auto BoxBuilder::height(Dimension d) -> BoxBuilder& {
    element_.layout.height = d;
    return *this;
}

auto BoxBuilder::min_width(Dimension d) -> BoxBuilder& {
    element_.layout.min_width = d;
    return *this;
}

auto BoxBuilder::min_height(Dimension d) -> BoxBuilder& {
    element_.layout.min_height = d;
    return *this;
}

auto BoxBuilder::max_width(Dimension d) -> BoxBuilder& {
    element_.layout.max_width = d;
    return *this;
}

auto BoxBuilder::max_height(Dimension d) -> BoxBuilder& {
    element_.layout.max_height = d;
    return *this;
}

auto BoxBuilder::gap(int g) -> BoxBuilder& {
    element_.layout.gap = g;
    return *this;
}

auto BoxBuilder::align_items(Align a) -> BoxBuilder& {
    element_.layout.align_items = a;
    return *this;
}

auto BoxBuilder::align_self(Align a) -> BoxBuilder& {
    element_.layout.align_self = a;
    return *this;
}

auto BoxBuilder::justify(Justify j) -> BoxBuilder& {
    element_.layout.justify = j;
    return *this;
}

auto BoxBuilder::overflow(Overflow o) -> BoxBuilder& {
    element_.overflow = o;
    return *this;
}

auto BoxBuilder::bg(Color c) -> BoxBuilder& {
    element_.style = element_.style.with_bg(c);
    return *this;
}

auto BoxBuilder::fg(Color c) -> BoxBuilder& {
    element_.style = element_.style.with_fg(c);
    return *this;
}

auto BoxBuilder::style(Style s) -> BoxBuilder& {
    element_.style = element_.style.merge(s);
    return *this;
}

// ============================================================================
// Factory functions
// ============================================================================

namespace detail {

auto box() -> BoxBuilder {
    return BoxBuilder{};
}

auto box(FlexStyle layout) -> BoxBuilder {
    return BoxBuilder{layout};
}

auto vstack() -> BoxBuilder {
    return BoxBuilder{FlexStyle{.direction = FlexDirection::Column}};
}

auto hstack() -> BoxBuilder {
    return BoxBuilder{FlexStyle{.direction = FlexDirection::Row}};
}

auto center() -> BoxBuilder {
    return BoxBuilder{FlexStyle{
        .direction   = FlexDirection::Column,
        .align_items = Align::Center,
        .justify     = Justify::Center,
    }};
}

} // namespace detail

// ============================================================================
// operator| — Pipe a Style onto an Element
// ============================================================================

Element operator|(Element elem, const Style& s) {
    visit_element(elem, [&](auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, BoxElement>) {
            node.style = node.style.merge(s);
        } else if constexpr (std::same_as<T, TextElement>) {
            node.style = node.style.merge(s);
        }
        // ElementList: no-op (style doesn't apply to fragments)
    });
    return elem;
}

} // namespace maya
