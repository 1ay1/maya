#pragma once
// maya::element::builder — Runtime element construction (internal)
//
// Low-level runtime builders used by maya::dsl to produce Element trees.
// Prefer the compile-time DSL (maya/dsl.hpp) for user-facing code:
//
//   using namespace maya::dsl;
//   constexpr auto ui = v(
//       t<"Hello"> | Bold | Fg<100, 180, 255>,
//       h(t<"A">, t<"B"> | Dim) | border_<Round> | pad<1>
//   );
//   maya::print(ui.build());
//
// These runtime functions (text(), box(), vstack(), hstack()) are retained
// as implementation details and for use inside dyn() escape hatches.

#include "element.hpp"

#include <concepts>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// For text() overloads that accept numeric types.
#include <charconv>
#include <array>

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
    std::same_as<std::remove_cvref_t<T>, ElementList> ||
    std::convertible_to<T, Element>;

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

    auto direction(FlexDirection d) -> BoxBuilder&;
    auto wrap(FlexWrap w) -> BoxBuilder&;
    auto padding(int all) -> BoxBuilder&;
    auto padding(int v, int h) -> BoxBuilder&;
    auto padding(int top, int right, int bottom, int left) -> BoxBuilder&;
    auto margin(int all) -> BoxBuilder&;
    auto margin(int v, int h) -> BoxBuilder&;
    auto margin(int top, int right, int bottom, int left) -> BoxBuilder&;
    auto border(BorderStyle bs) -> BoxBuilder&;
    auto border(BorderStyle bs, Color c) -> BoxBuilder&;
    auto border_color(Color c) -> BoxBuilder&;
    auto border_sides(BorderSides sides) -> BoxBuilder&;
    auto border_text(std::string_view content,
                     BorderTextPos pos = BorderTextPos::Top) -> BoxBuilder&;
    auto border_text(std::string_view content,
                     BorderTextPos pos,
                     BorderTextAlign align) -> BoxBuilder&;
    auto grow(float g = 1.0f) -> BoxBuilder&;
    auto shrink(float s) -> BoxBuilder&;
    auto basis(Dimension d) -> BoxBuilder&;
    auto width(Dimension d) -> BoxBuilder&;
    auto height(Dimension d) -> BoxBuilder&;
    auto min_width(Dimension d) -> BoxBuilder&;
    auto min_height(Dimension d) -> BoxBuilder&;
    auto max_width(Dimension d) -> BoxBuilder&;
    auto max_height(Dimension d) -> BoxBuilder&;
    auto gap(int g) -> BoxBuilder&;
    auto align_items(Align a) -> BoxBuilder&;
    auto align_self(Align a) -> BoxBuilder&;
    auto justify(Justify j) -> BoxBuilder&;
    auto overflow(Overflow o) -> BoxBuilder&;
    auto bg(Color c) -> BoxBuilder&;
    auto fg(Color c) -> BoxBuilder&;
    auto style(Style s) -> BoxBuilder&;

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

namespace detail {

/// Create a new BoxBuilder. Internal — users should use dsl::v() / dsl::h().
[[nodiscard]] auto box() -> BoxBuilder;

/// Create a new BoxBuilder pre-configured with a FlexStyle.
[[nodiscard]] auto box(FlexStyle layout) -> BoxBuilder;

} // namespace detail

// ============================================================================
// operator| — Pipe a Style onto an Element (internal, found via ADL)
// ============================================================================

[[nodiscard]] Element operator|(Element elem, const Style& s);

// ============================================================================
// Layout shortcuts (internal — used by DSL and tests)
// ============================================================================

namespace detail {

[[nodiscard]] auto vstack() -> BoxBuilder;
[[nodiscard]] auto hstack() -> BoxBuilder;
[[nodiscard]] auto center() -> BoxBuilder;

} // namespace detail

} // namespace maya
