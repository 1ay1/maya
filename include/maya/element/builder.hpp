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
#include <cstdint>
#include <cstdio>
#include <memory>
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
    // Second border-text slot (independent position+align). Use for a
    // right-aligned subtitle on the same edge as a left-aligned title:
    //   .border_text(" Title ", Top, Start)
    //   .border_text_end(" 87ms ", Top, End)
    auto border_text_end(std::string_view content,
                         BorderTextPos pos = BorderTextPos::Top,
                         BorderTextAlign align = BorderTextAlign::End) -> BoxBuilder&;
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

/// Create a z-stack: children layer on top of each other. The first child
/// determines the size; subsequent children paint on top, clipped to that size.
[[nodiscard]] inline Element zstack(std::vector<Element> layers) {
    BoxElement box;
    box.layout.direction = FlexDirection::Column;
    box.is_stack = true;
    box.children = std::move(layers);
    return Element{std::move(box)};
}

} // namespace detail

// ============================================================================
// ComponentBuilder - Fluent builder for ComponentElement
// ============================================================================
// Creates a lazy element that defers rendering until layout allocates a size.
// The render callback receives (width, height) and returns an Element.
//
// Usage:
//   component([](int w, int h) {
//       return LineChart({.series = data, .width = w, .height = h});
//   }).grow(1).border(BorderStyle::Round)

class ComponentBuilder {
    ComponentElement element_{};

public:
    explicit ComponentBuilder(std::function<Element(int, int)> render_fn)
    {
        element_.render = std::move(render_fn);
    }

    auto grow(float g = 1.0f) -> ComponentBuilder& {
        element_.layout.grow = g;
        return *this;
    }

    auto shrink(float s) -> ComponentBuilder& {
        element_.layout.shrink = s;
        return *this;
    }

    auto basis(Dimension d) -> ComponentBuilder& {
        element_.layout.basis = d;
        return *this;
    }

    auto width(Dimension d) -> ComponentBuilder& {
        element_.layout.width = d;
        return *this;
    }

    auto height(Dimension d) -> ComponentBuilder& {
        element_.layout.height = d;
        return *this;
    }

    auto min_width(Dimension d) -> ComponentBuilder& {
        element_.layout.min_width = d;
        return *this;
    }

    auto min_height(Dimension d) -> ComponentBuilder& {
        element_.layout.min_height = d;
        return *this;
    }

    auto max_width(Dimension d) -> ComponentBuilder& {
        element_.layout.max_width = d;
        return *this;
    }

    auto max_height(Dimension d) -> ComponentBuilder& {
        element_.layout.max_height = d;
        return *this;
    }

    auto padding(int all) -> ComponentBuilder& {
        element_.layout.padding = {all, all, all, all};
        return *this;
    }

    auto padding(int v, int h) -> ComponentBuilder& {
        element_.layout.padding = {v, h, v, h};
        return *this;
    }

    auto padding(int top, int right, int bottom, int left) -> ComponentBuilder& {
        element_.layout.padding = {top, right, bottom, left};
        return *this;
    }

    auto margin(int all) -> ComponentBuilder& {
        element_.layout.margin = {all, all, all, all};
        return *this;
    }

    auto align_self(Align a) -> ComponentBuilder& {
        element_.layout.align_self = a;
        return *this;
    }

    /// Supply an explicit measure callback (the component's natural size
    /// for layout). WITHOUT one, the framework auto-measures by RENDERING
    /// the component at an unbounded height (1<<20) and counting the rows
    /// it emits. That is correct for a content-sized render, but CATASTROPHIC
    /// for a HEIGHT-FILLING render that sizes its output to the `h` it is
    /// handed — measured at 1<<20 it would emit ~a million rows and report an
    /// absurd natural height, poisoning its flex-basis so grow can never
    /// expand it. A fill component MUST report a small finite basis here so
    /// flex-grow has something to grow FROM. Prefer the `fill()` factory,
    /// which wires this correctly for you.
    auto measure(std::function<Size(int max_width)> fn) -> ComponentBuilder& {
        element_.measure = std::move(fn);
        return *this;
    }

    // Set the content-stable cache key (Witness Chain).
    //
    // Non-empty CacheId values route this component through the
    // renderer's hash-keyed component_cache, so a fresh
    // ComponentElement value-copied through containers each frame
    // still hits the cells captured on the first paint. Construct
    // the id via `CacheIdBuilder{}.add(...).build()` — the typed
    // builder mixes type tags into the FNV-1a hash, so two ids
    // built from semantically-different inputs can't collide via
    // string-concatenation accidents (e.g. "tool:1:23" vs
    // "tool:12:3").
    //
    // An empty CacheId keeps the pointer-keyed behaviour and pays
    // the full render cost every frame.
    auto hash_id(CacheId id) -> ComponentBuilder& {
        element_.hash_id = id;
        return *this;
    }

    /// Implicit conversion to Element.
    operator Element() const& {
        return Element{element_};
    }

    operator Element() && {
        return Element{std::move(element_)};
    }
};

// ============================================================================
// component() - Factory for lazy elements
// ============================================================================

namespace detail {

/// Create a lazy element that receives its allocated size before rendering.
/// The callback is called with (width, height) during painting and must
/// return an Element tree that fits within those bounds.
[[nodiscard]] inline auto component(std::function<Element(int, int)> render_fn)
    -> ComponentBuilder
{
    return ComponentBuilder{std::move(render_fn)};
}

// ============================================================================
// fill() - A component that FILLS the size flex allocates to it
// ============================================================================
// The height-responsive (and width-responsive) counterpart to component().
//
// component() sizes to CONTENT: its natural height is whatever its render
// produces, and flex positions it at that size. fill() sizes to its SLOT:
// it grows to consume the space its flex container gives it, and its render
// callback receives the REAL allocated (w, h) at paint time. Size your
// graph / canvas / gauge to `h` and it always fits the box exactly — no
// hand-computed row budget threaded down from the parent, so the estimate
// can never drift from what the layout engine actually allocates.
//
//   // A graph that always fills the space left after the meters:
//   v(
//       fill([&](int w, int h){ return area_chart(data, w, h); }),  // fills
//       meter_row(), meter_row()                                    // natural
//   ) | height(N)     // definite parent → the fill grows into the slack
//
// Mechanics (why this needs a factory, not just component().grow(1)):
//   * A plain grow(1) component with no measure is auto-measured by
//     rendering at an unbounded height and counting rows — a fill render
//     emits ~2^20 rows there and reports a nonsense basis, so grow can
//     never expand it. fill() installs a measure that reports a small
//     fixed minimum (min_w x min_h) instead, giving grow a finite basis.
//   * fill() sets grow(1) so it claims the container's free space.
//
// Requirement: the container must be DEFINITE on the fill axis for grow to
// distribute — an explicit height()/width() on an ancestor, or cross-axis
// stretch inherited from a definite-size parent. Same rule as any grow
// child; an auto-sized container has no free space to hand out. When the
// slot is too small the callback simply receives a small `h` (down to
// min_h) — collapse gracefully there (e.g. return blank() below a floor).
[[nodiscard]] inline auto fill(std::function<Element(int w, int h)> render_fn,
                               int min_w = 0, int min_h = 1)
    -> ComponentBuilder
{
    ComponentBuilder b{std::move(render_fn)};
    b.measure([min_w, min_h](int max_width) -> Size {
        int w = max_width > 0 ? max_width : 0;
        if (w < min_w) w = min_w;
        return {Columns{w}, Rows{min_h < 1 ? 1 : min_h}};
    });
    b.grow(1.0f);
    return b;
}

// ============================================================================
// nothing() - Zero-height placeholder (transparent empty fragment)
// ============================================================================
// Use for view slots that should consume no rows when their content is
// absent (e.g. an in-flight thinking block when the agent isn't thinking).
// Returns an empty ElementList — a flex fragment with no children, which
// the layout engine treats as zero rows / zero columns. Distinct from
// blank() which is a one-row spacer.

[[nodiscard]] inline Element nothing() {
    return Element{ElementList{}};
}

// ============================================================================
// list_ref() - Borrow a vector of Elements without copying
// ============================================================================
// Renders the pointed-to vector as a transparent fragment, identical
// in semantics to wrapping the vector in an ElementList — but without
// the per-frame deep copy. Suitable when the application's Model
// holds a stable vector (e.g. frozen scrollback) and view() is
// called synchronously between updates so the pointer remains valid.

[[nodiscard]] inline Element list_ref(const std::vector<Element>* items) {
    return Element{ElementListRef{items}};
}

[[nodiscard]] inline Element list_ref(const std::vector<Element>& items) {
    return Element{ElementListRef{&items}};
}

// ============================================================================
// ledger_ref() - Borrow a ScrollbackLedger's sealed blocks (measured)
// ============================================================================
// Same zero-copy semantics as list_ref over ledger.elements(), PLUS the
// paint pass records each block's laid-out height back into the ledger
// every frame (Witness Chain — Trim Accounting). Hosts that front-trim
// a sealed prefix MUST render it through this so ledger.harvest() mints
// commit counts from maya's own measurements. Declared here; defined in
// dsl.hpp region after ScrollbackLedger is complete via the include in
// element.hpp consumers — the ledger header is standalone, so include
// it directly.

[[nodiscard]] Element ledger_ref(const ScrollbackLedger& ledger);

} // namespace detail

// ============================================================================
// Public API — promote runtime builders out of detail::
// ============================================================================
// These are the runtime counterpart to the compile-time DSL (dsl::v, dsl::h).
// Use these when you need runtime-configured borders, colors, padding, etc.
//
//   box().border(Round).border_color(status_color)
//       .border_text(title, BorderTextPos::Top)
//       .padding(0, 1, 0, 1)(children)

using detail::box;
using detail::vstack;
using detail::hstack;
using detail::center;

} // namespace maya
