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

#include <atomic>
#include <concepts>
#include <cstdint>
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

    // Body defined out-of-line below, after Element is a complete type. If
    // the body lived in-class, clang's parsing of the non-dependent
    // `items.reserve(...)` / `items.emplace_back(...)` calls would force
    // instantiation of vector<Element> members (e.g. `_S_max_size`, which
    // does `sizeof(Element)`) while Element is still incomplete here.
    // Defining the template after the Element definition below sidesteps
    // that — the body is only ever instantiated at user call sites, where
    // Element is complete.
    template <typename... Args>
        requires (sizeof...(Args) > 0)
    explicit ElementList(Args&&... args);
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

namespace detail {

// Per-process unique generation for ComponentElement instances. The
// renderer's cross-frame component_cache keys on
// (ComponentElement*, generation) — the pointer alone is not enough,
// because the allocator can reuse a freed ComponentElement's address
// for a fresh one and the cache would alias the new instance to the
// old's render output. The generation is stamped at default
// construction; copy/move propagate it (a value-copied
// ComponentElement is the same logical instance — no fresh
// generation), so address-stable storage like a vector slot whose
// element is mutated in place keeps its cache identity.
//
// Atomic relaxed: any monotonic uniqueness ordering across threads is
// sufficient — the cache only needs to detect "this isn't the same
// instance I cached before," not establish happens-before with
// anything else.
[[nodiscard]] inline std::uint64_t next_component_generation() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail

struct ComponentElement {
    /// Called during painting with the allocated width and height.
    std::function<Element(int width, int height)> render;

    /// Optional custom measure function for layout. When provided, the
    /// layout engine calls this instead of the default {max_width, 1}.
    /// This lets components like Scrollable report their content height
    /// so they size correctly in auto_height (unconstrained) layouts.
    std::function<Size(int max_width)> measure = nullptr;

    /// Layout properties — participates in flexbox just like BoxElement.
    FlexStyle layout{};

    /// Per-instance identity for the renderer's cross-frame
    /// component_cache. See detail::next_component_generation() for
    /// the rationale; in short: ComponentElement* alone is not a
    /// stable cache key because the allocator can reuse an address.
    /// Stamped at default-construction; copy/move propagate so a
    /// ComponentElement sitting in a stable slot (a Box's children
    /// vector that doesn't reallocate, the variant storage of a
    /// member-held Element) keeps its identity across mutations of
    /// neighbouring slots.
    std::uint64_t generation = detail::next_component_generation();
};

// ============================================================================
// ElementListRef - Non-owning fragment for stable application-side data
// ============================================================================
// Like ElementList, but holds a pointer to an externally-owned vector
// rather than copying it. The application guarantees that the pointed-to
// vector outlives the rendering pass (typically because it lives in the
// application's Model and view() runs synchronously between updates).
//
// Why it exists: the dominant cost in long-running streaming sessions
// is `Element{ElementList{model.frozen}}` deep-copying a 240-element
// vector every frame. With ElementListRef the application can write
// `Element{ElementListRef{&model.frozen}}` and pay zero copy cost —
// the renderer treats it as a transparent fragment, identical to
// ElementList semantically, and reads the items in place.
//
// Safety: the pointer must remain valid for the duration of the
// render pass. Since maya's runtime calls update() and view() on a
// single thread (the UI thread) and renders synchronously, the
// application-side vector stored in Model is automatically stable
// for the duration of view() → render().

struct ElementListRef {
    const std::vector<Element>* items_ref = nullptr;
};

// ============================================================================
// Element - The tagged union of all element types
// ============================================================================
// Wraps std::variant for type-safe, zero-overhead dispatch. Every node in a
// maya UI tree is one of these.
//
// BoxElement       - A flex container with children, border, padding, etc.
// TextElement      - A leaf node displaying styled text.
// ElementList      - A transparent owned fragment (vector of Element).
// ElementListRef   - A transparent borrowed fragment (pointer to a vector
//                    owned by the application). Zero-copy alternative to
//                    ElementList for stable application-side data.
// ComponentElement - A lazy element that renders after receiving its size.

struct Element {
    using Variant = std::variant<BoxElement, TextElement, ElementList,
                                 ElementListRef, ComponentElement>;

    Variant inner;

    // -- Implicit converting constructors from each alternative ---------------

    Element(BoxElement b) : inner(std::move(b)) {}
    Element(TextElement t) : inner(std::move(t)) {}
    Element(ElementList l) : inner(std::move(l)) {}
    Element(ElementListRef r) : inner(r) {}
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

// -- Deferred ElementList constructors (Element is now complete) ---------------

inline ElementList::ElementList(std::vector<Element> elems)
    : items(std::move(elems)) {}

template <typename... Args>
    requires (sizeof...(Args) > 0)
ElementList::ElementList(Args&&... args) {
    items.reserve(sizeof...(Args));
    (items.emplace_back(std::forward<Args>(args)), ...);
}

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
