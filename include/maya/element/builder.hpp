#pragma once
// maya::element::builder - Declarative DSL for constructing element trees
//
// The crown jewel of the maya API. BoxBuilder provides a fluent, chainable
// interface for assembling UI trees that reads almost like a markup language:
//
//   box().direction(Column).border(Round).padding(1)(
//       text("Hello", bold_style | fg_red),
//       box().direction(Row).gap(1)(
//           text("Left"),
//           text("Right", dim_style)
//       )
//   )
//
// The trick: BoxBuilder accumulates properties via chained method calls,
// then operator() finalizes the element by accepting children. An implicit
// conversion to Element handles the no-children (self-closing) case.

#include "element.hpp"

#include <concepts>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace maya {

// ============================================================================
// Concepts for operator() child argument deduction
// ============================================================================

// Forward declaration so concepts can reference it.
class BoxBuilder;

/// True for types that are already an Element or implicitly convert to one.
template <typename T>
concept ElementConvertible =
    std::same_as<std::remove_cvref_t<T>, Element>    ||
    std::same_as<std::remove_cvref_t<T>, BoxElement>  ||
    std::same_as<std::remove_cvref_t<T>, TextElement> ||
    std::same_as<std::remove_cvref_t<T>, ElementList>;

/// True for BoxBuilder (needs special handling: finalize with no children).
template <typename T>
concept IsBoxBuilder = std::same_as<std::remove_cvref_t<T>, BoxBuilder>;

/// True for string-like types that should be auto-wrapped in text().
template <typename T>
concept TextConvertible =
    !ElementConvertible<T> &&
    !IsBoxBuilder<T> &&
    (std::convertible_to<T, std::string_view> ||
     std::same_as<std::remove_cvref_t<T>, std::string>);

/// True for ranges of Element (e.g., vector<Element>, span<Element>).
template <typename T>
concept ElementRange =
    !ElementConvertible<T> &&
    !IsBoxBuilder<T> &&
    !TextConvertible<T> &&
    std::ranges::range<T> &&
    std::convertible_to<std::ranges::range_value_t<T>, Element>;

/// Anything that operator() can accept as a child.
template <typename T>
concept ChildArg =
    ElementConvertible<T> || IsBoxBuilder<T> || TextConvertible<T> || ElementRange<T>;

// ============================================================================
// BoxBuilder - Fluent builder for BoxElement
// ============================================================================

class BoxBuilder {
    BoxElement element_{};

    // -- Internal: push a single child from a ChildArg ----------------------

    void push_child(Element elem) {
        element_.children.push_back(std::move(elem));
    }

    void push_child(BoxElement elem) {
        element_.children.emplace_back(std::move(elem));
    }

    void push_child(TextElement elem) {
        element_.children.emplace_back(std::move(elem));
    }

    void push_child(ElementList elem) {
        element_.children.emplace_back(std::move(elem));
    }

    // Resolve a single ChildArg into Element(s) and append.
    template <ChildArg T>
    void append_child(T&& child) {
        if constexpr (ElementConvertible<T>) {
            push_child(std::forward<T>(child));
        } else if constexpr (IsBoxBuilder<T>) {
            push_child(static_cast<Element>(std::forward<T>(child)));
        } else if constexpr (TextConvertible<T>) {
            push_child(Element{TextElement{.content = std::string{std::string_view{child}}}});
        } else if constexpr (ElementRange<T>) {
            for (auto&& item : child) {
                push_child(Element{std::forward<decltype(item)>(item)});
            }
        }
    }

public:
    BoxBuilder() = default;

    explicit BoxBuilder(FlexStyle layout) {
        element_.layout = layout;
    }

    // -- Property setters (return *this for chaining) -----------------------

    auto direction(FlexDirection d) -> BoxBuilder& {
        element_.layout.direction = d;
        return *this;
    }

    auto wrap(FlexWrap w) -> BoxBuilder& {
        element_.layout.wrap = w;
        return *this;
    }

    auto padding(int all) -> BoxBuilder& {
        element_.layout.padding = Edges<int>{all};
        return *this;
    }

    auto padding(int v, int h) -> BoxBuilder& {
        element_.layout.padding = Edges<int>{v, h};
        return *this;
    }

    auto padding(int top, int right, int bottom, int left) -> BoxBuilder& {
        element_.layout.padding = Edges<int>{top, right, bottom, left};
        return *this;
    }

    auto margin(int all) -> BoxBuilder& {
        element_.layout.margin = Edges<int>{all};
        return *this;
    }

    auto margin(int v, int h) -> BoxBuilder& {
        element_.layout.margin = Edges<int>{v, h};
        return *this;
    }

    auto margin(int top, int right, int bottom, int left) -> BoxBuilder& {
        element_.layout.margin = Edges<int>{top, right, bottom, left};
        return *this;
    }

    auto border(BorderStyle bs) -> BoxBuilder& {
        element_.border.style = bs;
        element_.border.sides = BorderSides::all();
        return *this;
    }

    auto border(BorderStyle bs, Color c) -> BoxBuilder& {
        element_.border.style = bs;
        element_.border.sides = BorderSides::all();
        element_.border.colors = BorderColors::uniform(c);
        return *this;
    }

    auto border_color(Color c) -> BoxBuilder& {
        element_.border.colors = BorderColors::uniform(c);
        return *this;
    }

    auto border_sides(BorderSides sides) -> BoxBuilder& {
        element_.border.sides = sides;
        return *this;
    }

    auto border_text(std::string_view content,
                     BorderTextPos pos = BorderTextPos::Top) -> BoxBuilder& {
        element_.border.text = BorderText{
            .content  = std::string{content},
            .position = pos,
        };
        return *this;
    }

    auto border_text(std::string_view content,
                     BorderTextPos pos,
                     BorderTextAlign align) -> BoxBuilder& {
        element_.border.text = BorderText{
            .content  = std::string{content},
            .position = pos,
            .align    = align,
        };
        return *this;
    }

    auto grow(float g = 1.0f) -> BoxBuilder& {
        element_.layout.grow = g;
        return *this;
    }

    auto shrink(float s) -> BoxBuilder& {
        element_.layout.shrink = s;
        return *this;
    }

    auto basis(Dimension d) -> BoxBuilder& {
        element_.layout.basis = d;
        return *this;
    }

    auto width(Dimension d) -> BoxBuilder& {
        element_.layout.width = d;
        return *this;
    }

    auto height(Dimension d) -> BoxBuilder& {
        element_.layout.height = d;
        return *this;
    }

    auto min_width(Dimension d) -> BoxBuilder& {
        element_.layout.min_width = d;
        return *this;
    }

    auto min_height(Dimension d) -> BoxBuilder& {
        element_.layout.min_height = d;
        return *this;
    }

    auto max_width(Dimension d) -> BoxBuilder& {
        element_.layout.max_width = d;
        return *this;
    }

    auto max_height(Dimension d) -> BoxBuilder& {
        element_.layout.max_height = d;
        return *this;
    }

    auto gap(int g) -> BoxBuilder& {
        element_.layout.gap = g;
        return *this;
    }

    auto align_items(Align a) -> BoxBuilder& {
        element_.layout.align_items = a;
        return *this;
    }

    auto align_self(Align a) -> BoxBuilder& {
        element_.layout.align_self = a;
        return *this;
    }

    auto justify(Justify j) -> BoxBuilder& {
        element_.layout.justify = j;
        return *this;
    }

    auto overflow(Overflow o) -> BoxBuilder& {
        element_.overflow = o;
        return *this;
    }

    auto bg(Color c) -> BoxBuilder& {
        element_.style.bg = c;
        return *this;
    }

    auto fg(Color c) -> BoxBuilder& {
        element_.style.fg = c;
        return *this;
    }

    auto style(Style s) -> BoxBuilder& {
        element_.style = s;
        return *this;
    }

    // -- Finalize with children via operator() ------------------------------

    /// Accept zero or more children and produce the final Element.
    /// Each child can be an Element, BoxBuilder, string, or range<Element>.
    template <ChildArg... Children>
    auto operator()(Children&&... children) -> Element {
        (append_child(std::forward<Children>(children)), ...);
        return Element{std::move(element_)};
    }

    /// Implicit conversion to Element (no-children / self-closing form).
    /// Allows: `auto e = box().grow();` without calling operator().
    operator Element() const& {
        return Element{element_};
    }

    operator Element() && {
        return Element{std::move(element_)};
    }

    // -- Access to the underlying element (escape hatch) --------------------

    [[nodiscard]] const BoxElement& get() const noexcept { return element_; }
    [[nodiscard]] BoxElement& get() noexcept { return element_; }
};

// ============================================================================
// Factory functions
// ============================================================================

/// Create a new BoxBuilder with default properties.
[[nodiscard]] inline auto box() -> BoxBuilder {
    return BoxBuilder{};
}

/// Create a new BoxBuilder pre-configured with a FlexStyle.
[[nodiscard]] inline auto box(FlexStyle layout) -> BoxBuilder {
    return BoxBuilder{std::move(layout)};
}

/// Create a text Element from a string_view.
[[nodiscard]] inline auto text(std::string_view content, Style s = {}) -> Element {
    return Element{TextElement{
        .content = std::string{content},
        .style   = s,
    }};
}

/// Create a text Element with a specific wrap mode.
[[nodiscard]] inline auto text(std::string_view content, Style s, TextWrap w) -> Element {
    return Element{TextElement{
        .content = std::string{content},
        .style   = s,
        .wrap    = w,
    }};
}

// ============================================================================
// Separator and spacer helpers
// ============================================================================

/// A flexible spacer that absorbs remaining space (flex_grow: 1).
/// Useful for pushing siblings to opposite ends of a container.
[[nodiscard]] inline auto spacer() -> Element {
    return box().grow(1.0f);
}

/// A horizontal or vertical line separator.
/// Renders as a border-only box that stretches across the cross axis.
[[nodiscard]] inline auto separator(BorderStyle bs = BorderStyle::Single) -> Element {
    return box()
        .border(bs)
        .border_sides(BorderSides::horizontal());
}

/// Vertical separator variant.
[[nodiscard]] inline auto vseparator(BorderStyle bs = BorderStyle::Single) -> Element {
    return box()
        .border(bs)
        .border_sides(BorderSides::vertical());
}

// ============================================================================
// Convenience: list builder from a range
// ============================================================================
// Turns a range of items into an ElementList by mapping each item through
// a projection function that returns an Element.
//
// Usage:
//   auto items = std::vector<std::string>{"a", "b", "c"};
//   box().direction(Column)(
//       map_elements(items, [](const auto& s) { return text(s); })
//   )

template <std::ranges::range R, typename Proj>
    requires std::invocable<Proj, std::ranges::range_value_t<R>> &&
             std::convertible_to<std::invoke_result_t<Proj, std::ranges::range_value_t<R>>, Element>
[[nodiscard]] auto map_elements(R&& range, Proj proj) -> ElementList {
    std::vector<Element> items;
    if constexpr (std::ranges::sized_range<R>) {
        items.reserve(std::ranges::size(range));
    }
    for (auto&& val : range) {
        items.emplace_back(proj(std::forward<decltype(val)>(val)));
    }
    return ElementList{std::move(items)};
}

} // namespace maya
