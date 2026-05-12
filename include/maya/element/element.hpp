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
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
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

// Mint a stable identity for a shared_ptr<const Element>, keyed on the
// control block (so all copies of the same shared_ptr hash to the same
// id, and a freshly-allocated shared_ptr that happens to reuse a
// previously-freed raw data pointer is recognised as DIFFERENT).
//
// Why this isn't just `sp.get()`: the implicit ctor below derives a
// `cache_id` string used by the renderer's content-keyed
// component_cache. If two shared_ptrs to *different* logical content
// happen to land at the same raw address (allocator recycling — common
// when shared_ptrs churn during streaming/scrolling), a get()-derived
// cache_id collides and the renderer blits stale cells from the dead
// shared_ptr's cache entry under the new one. Observed in agentty as
// a hard-to-reproduce ghost composer + footer that resize cleared.
//
// The map keys weak_ptrs by std::owner_less, which compares CONTROL
// BLOCKS (the bookkeeping object make_shared allocates alongside the
// managed object; lifetime spans all shared_ptr/weak_ptr copies, freed
// only when the last reference is gone). Control-block addresses can
// still be recycled by the system allocator, but a new control block
// allocated at a previously-used address is a different node by
// owner_less — owner_before reads the internal pointer's identity,
// not just its bits.
//
// Bounded growth: the map is thread_local and we sweep expired entries
// when it grows past a soft cap (1024). Typical session sizes stay
// well under that; the sweep is O(N) but only runs at the cap so
// amortised per-call cost is O(1).
[[nodiscard]] inline std::uint64_t id_for_shared(
    const std::shared_ptr<const Element>& sp)
{
    using Key = std::weak_ptr<const Element>;
    using Cmp = std::owner_less<Key>;
    thread_local std::map<Key, std::uint64_t, Cmp> ids;
    static std::atomic<std::uint64_t> next_id{1};

    Key key = sp;
    if (auto it = ids.find(key); it != ids.end()) {
        return it->second;
    }
    if (ids.size() >= 1024) {
        for (auto it = ids.begin(); it != ids.end(); ) {
            if (it->first.expired()) it = ids.erase(it);
            else ++it;
        }
    }
    auto id = next_id.fetch_add(1, std::memory_order_relaxed);
    ids.emplace(std::move(key), id);
    return id;
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

    /// Optional content-stable cache key. Empty by default — pointer
    /// keying takes over and the lifecycle above applies.
    ///
    /// Set this to a host-meaningful identifier (e.g. a (thread, msg)
    /// slug) when the component represents content that's logically
    /// the same across frames even though the ComponentElement
    /// instance gets value-copied through containers each frame. The
    /// renderer's cross-frame component cache prefers this key when
    /// non-empty, so the new copies still hit the cache produced by
    /// the original — turning per-frame deep-copy churn (a fresh
    /// ComponentElement* every frame, never matching anything in the
    /// pointer-keyed cache) into O(1) measure + paint.
    ///
    /// Equality is by string value; pick something cheap to compare
    /// (short, hashable). Collisions across unrelated components map
    /// them to the same cache slot — be careful that your id
    /// uniquely identifies the rendered content within the running
    /// app. Empty string means "use pointer keying," matching the
    /// pre-cache_id behaviour for any caller that hasn't opted in.
    std::string cache_id;
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

    // Implicit conversion from `shared_ptr<const Element>`: the
    // application keeps a long-lived Element (typically a memoised
    // settled item — a settled chat turn, a frozen list row) on the
    // heap via shared_ptr; handing maya the shared_ptr instead of
    // the Element by value lets the renderer key its cross-frame
    // cache on the underlying pointer address (stable across copies
    // of THIS wrapper) and skip rerendering / relayout / repainting
    // the subtree on every frame. The caller writes naturally —
    // `cfg.built_turns.push_back({my_sp, continuation})` — and never
    // sees the cache infrastructure.
    //
    // Lifetime: the constructed wrapper holds a copy of the
    // shared_ptr, so the underlying Element stays alive as long as
    // any frame still references the wrapper. Width changes
    // automatically invalidate the cache entry through the existing
    // (id, width) keying inside the renderer.
    Element(std::shared_ptr<const Element> sp) {
        ComponentElement c;
        // Stable cross-frame identity keyed on the shared_ptr's control
        // block — copies of the same shared_ptr produce the same id;
        // distinct shared_ptrs produce distinct ids even if their raw
        // data pointers alias (allocator recycling).
        //
        // The earlier version of this ctor derived cache_id from
        // sp.get() directly, which collided cache entries across the
        // lifetimes of unrelated shared_ptrs that happened to land at
        // the same heap address. That surfaced as the streaming
        // composer / footer ghost: a cell strip captured for a
        // long-dead shared_ptr blitted under a freshly-allocated
        // shared_ptr at the same address, painting old content over
        // the live layout's position.
        char buf[24];
        std::snprintf(buf, sizeof(buf), "#%lx",
                      static_cast<unsigned long>(detail::id_for_shared(sp)));
        c.cache_id = std::string{buf};
        c.render = [sp = std::move(sp)](int /*w*/, int /*h*/) -> Element {
            return *sp;
        };
        inner = std::move(c);
    }

    // Non-const-shared-ptr convenience overload. C++ won't chain the
    // standard-library `shared_ptr<T>` → `shared_ptr<const T>`
    // conversion with the user-defined `shared_ptr<const Element>` →
    // `Element` ctor across a brace-enclosed init (only one
    // user-defined conversion is allowed in such contexts). Provide
    // a direct overload so the natural call site
    // `{my_sp_to_element, continuation_flag}` compiles without the
    // caller adding an explicit cast.
    Element(std::shared_ptr<Element> sp)
        : Element(std::shared_ptr<const Element>{std::move(sp)}) {}

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
