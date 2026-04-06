#pragma once
// maya::element::element - The Element variant type and visitor utilities
//
// Element is the core node type of every UI tree. It wraps a closed sum type
// (std::variant) over all concrete element kinds. By being a proper struct
// rather than a type alias, Element can be forward-declared — breaking the
// circular dependency with BoxElement (which holds vector<Element>).
//
// Include order is critical here:
//   1. Forward-declare Element (so box.hpp can use vector<Element>)
//   2. Include box.hpp and text.hpp (defines BoxElement, TextElement)
//   3. Define ElementList (needs vector<Element>, which works with incomplete Element)
//   4. Define Element (now all variant alternatives are complete types)

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace maya {

// ============================================================================
// Step 1: Forward-declare Element so that box.hpp / text.hpp can reference it
// ============================================================================

struct Element;
struct ElementList;

} // namespace maya

// ============================================================================
// Step 2: Include concrete element headers that depend on forward-declared Element
// ============================================================================

#include "box.hpp"
#include "text.hpp"

// ============================================================================
// Step 3-4: Define ElementList and Element now that all types are available
// ============================================================================

namespace maya {

// ============================================================================
// ElementList - A transparent grouping of elements (fragment)
// ============================================================================
// When you want to return multiple elements without an enclosing box,
// wrap them in an ElementList. Layout treats these as if the children
// were spliced directly into the parent's child list.

struct ElementList {
    std::vector<Element> items;

    ElementList() = default;

    explicit ElementList(std::vector<Element> elems);

    template <typename... Args>
        requires (sizeof...(Args) > 0)
    explicit ElementList(Args&&... args) {
        items.reserve(sizeof...(Args));
        (items.emplace_back(std::forward<Args>(args)), ...);
    }
};

// ============================================================================
// ComponentElement - A lazy element that renders after layout allocation
// ============================================================================
// Like a GTK widget: receives its allocated size before rendering.
// The render callback is called during painting with the final (width, height)
// and must return an Element tree that fits within those bounds.
//
// Usage with the DSL:
//   component([](int w, int h) {
//       return LineChart({.series = ..., .width = w, .height = h});
//   }).grow(1)

struct ComponentElement {
    /// Called during painting with the allocated width and height.
    std::function<Element(int width, int height)> render;

    /// Layout properties — participates in flexbox just like BoxElement.
    FlexStyle layout{};
};

// ============================================================================
// Element - The tagged union of all element types
// ============================================================================
// Wraps std::variant for type-safe, zero-overhead dispatch. Every node in a
// maya UI tree is one of these.
//
// BoxElement       - A flex container with children, border, padding, etc.
// TextElement      - A leaf node displaying styled text.
// ElementList      - A transparent fragment (vector of Element).
// ComponentElement - A lazy element that renders after receiving its size.

struct Element {
    using Variant = std::variant<BoxElement, TextElement, ElementList, ComponentElement>;

    Variant inner;

    // -- Implicit converting constructors from each alternative ---------------

    Element(BoxElement b) : inner(std::move(b)) {}
    Element(TextElement t) : inner(std::move(t)) {}
    Element(ElementList l) : inner(std::move(l)) {}
    Element(ComponentElement c) : inner(std::move(c)) {}

    // Default: empty text element
    Element() : inner(TextElement{}) {}

    Element(const Element&) = default;
    Element(Element&&) noexcept = default;
    Element& operator=(const Element&) = default;
    Element& operator=(Element&&) noexcept = default;
    ~Element() = default;

    // Satisfies the dsl::Node concept — an Element IS a built node.
    [[nodiscard]] Element build() const { return *this; }
};

// -- Deferred ElementList constructor (Element is now complete) ----------------

inline ElementList::ElementList(std::vector<Element> elems)
    : items(std::move(elems)) {}

// ============================================================================
// visit_element - Convenience wrapper around std::visit for Element
// ============================================================================

template <typename Visitor>
decltype(auto) visit_element(Element& elem, Visitor&& vis) {
    return std::visit(std::forward<Visitor>(vis), elem.inner);
}

template <typename Visitor>
decltype(auto) visit_element(const Element& elem, Visitor&& vis) {
    return std::visit(std::forward<Visitor>(vis), elem.inner);
}

template <typename Visitor>
decltype(auto) visit_element(Element&& elem, Visitor&& vis) {
    return std::visit(std::forward<Visitor>(vis), std::move(elem.inner));
}

// ============================================================================
// Type query helpers
// ============================================================================

[[nodiscard]] inline bool is_box(const Element& elem) noexcept {
    return std::holds_alternative<BoxElement>(elem.inner);
}

[[nodiscard]] inline bool is_text(const Element& elem) noexcept {
    return std::holds_alternative<TextElement>(elem.inner);
}

[[nodiscard]] inline bool is_list(const Element& elem) noexcept {
    return std::holds_alternative<ElementList>(elem.inner);
}

[[nodiscard]] inline bool is_component(const Element& elem) noexcept {
    return std::holds_alternative<ComponentElement>(elem.inner);
}

// ============================================================================
// Safe accessors (return pointer, nullptr on type mismatch)
// ============================================================================

[[nodiscard]] inline BoxElement* as_box(Element& elem) noexcept {
    return std::get_if<BoxElement>(&elem.inner);
}

[[nodiscard]] inline const BoxElement* as_box(const Element& elem) noexcept {
    return std::get_if<BoxElement>(&elem.inner);
}

[[nodiscard]] inline TextElement* as_text(Element& elem) noexcept {
    return std::get_if<TextElement>(&elem.inner);
}

[[nodiscard]] inline const TextElement* as_text(const Element& elem) noexcept {
    return std::get_if<TextElement>(&elem.inner);
}

[[nodiscard]] inline ElementList* as_list(Element& elem) noexcept {
    return std::get_if<ElementList>(&elem.inner);
}

[[nodiscard]] inline const ElementList* as_list(const Element& elem) noexcept {
    return std::get_if<ElementList>(&elem.inner);
}

} // namespace maya
