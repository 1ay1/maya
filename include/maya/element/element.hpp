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
class ScrollbackLedger;

} // namespace maya

// ============================================================================
// Step 2: Include concrete element headers that depend on forward-declared Element
// ============================================================================

#include "box.hpp"
#include "text.hpp"
#include "../render/cache_id.hpp"

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
// `hash_id` (CacheId) used by the renderer's hash-keyed
// component_cache. If two shared_ptrs to *different* logical content
// happen to land at the same raw address (allocator recycling — common
// when shared_ptrs churn during streaming/scrolling), a get()-derived
// id would collide and the renderer would blit stale cells from the
// dead shared_ptr's cache entry under the new one. Observed in agentty
// as a hard-to-reproduce ghost composer + footer that resize cleared.
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
// Bounded growth: the map is thread_local and we evict expired entries
// along three paths:
//   1. Inline on lookup: if `find` lands on a node whose weak_ptr has
//      expired, erase it immediately and treat the call as a miss.
//      Costs one O(log N) erase to avoid the entry surviving until the
//      next bulk sweep.
//   2. Incremental on miss: every kIncSweepEvery inserts, scan a
//      bounded prefix of the map (kIncSweepBudget entries) and erase
//      any expired weak_ptrs we hit. Spreads the O(N) bulk-sweep cost
//      across many calls so the p99 latency stays flat even on a
//      low-power device (Raspberry Pi) where a single 1024-entry
//      sweep is user-visible.
//   3. Backstop bulk sweep: at the hard cap (kHardCap), do the
//      original full scan. Should rarely trigger now that 1+2 keep
//      the map drained, but stays in place to bound worst-case
//      memory regardless of churn pattern.
// Typical session sizes stay well under the cap; per-call cost is
// O(log N) lookup + amortised O(1) sweep work.
[[nodiscard]] inline std::uint64_t id_for_shared(
    const std::shared_ptr<const Element>& sp)
{
    using Key = std::weak_ptr<const Element>;
    using Cmp = std::owner_less<Key>;
    thread_local std::map<Key, std::uint64_t, Cmp> ids;
    // Sweep cursor for the incremental scan. We resume from the entry
    // following the previous batch's last touched node on each tick,
    // so over time every entry is visited even though no single call
    // walks the whole map. Reset to begin() when it falls off the end.
    thread_local typename std::map<Key, std::uint64_t, Cmp>::iterator
        sweep_cursor = ids.end();
    thread_local std::size_t inserts_since_sweep = 0;
    static std::atomic<std::uint64_t> next_id{1};

    constexpr std::size_t kIncSweepEvery  = 64;     // tick every 64 inserts
    constexpr std::size_t kIncSweepBudget = 16;     // visit ≤ 16 entries / tick
    constexpr std::size_t kHardCap        = 1024;   // unchanged backstop

    Key key = sp;
    if (auto it = ids.find(key); it != ids.end()) {
        // Inline erase-on-expired. owner_less treats a freed control
        // block as distinct from a fresh one at the same address, so
        // an expired hit means the OLD shared_ptr we cached identity
        // for is gone; the new sp is logically different and must
        // mint a fresh id. Erase before falling through to the
        // insert path so the stale entry doesn't accumulate.
        if (it->first.expired()) {
            if (sweep_cursor == it) ++sweep_cursor;   // don't dangle the cursor
            ids.erase(it);
        } else {
            return it->second;
        }
    }

    // Incremental sweep tick. Amortises the bulk-sweep O(N) into
    // O(kIncSweepBudget) per tick.
    if (++inserts_since_sweep >= kIncSweepEvery) {
        inserts_since_sweep = 0;
        if (sweep_cursor == ids.end()) sweep_cursor = ids.begin();
        for (std::size_t budget = 0;
             budget < kIncSweepBudget && sweep_cursor != ids.end();
             ++budget)
        {
            if (sweep_cursor->first.expired()) {
                sweep_cursor = ids.erase(sweep_cursor);
            } else {
                ++sweep_cursor;
            }
        }
    }

    // Hard-cap backstop. Pathological churn can still race ahead of
    // the incremental sweep — keep the full scan as a guarantee.
    if (ids.size() >= kHardCap) {
        for (auto it = ids.begin(); it != ids.end(); ) {
            if (it->first.expired()) it = ids.erase(it);
            else ++it;
        }
        sweep_cursor = ids.end();   // iterators invalidated
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

    /// Content-stable cache identity (Witness Chain).
    ///
    /// Default empty — pointer keying takes over and the
    /// lifecycle above applies.
    ///
    /// Set this to a typed `CacheId` (constructed via
    /// `CacheIdBuilder{}.add(...).build()`) when the component
    /// represents content that's logically the same across frames
    /// even though the ComponentElement instance gets value-copied
    /// through containers each frame. The renderer's cross-frame
    /// component cache prefers this key when non-empty, so the new
    /// copies still hit the cache produced by the original —
    /// turning per-frame deep-copy churn (a fresh ComponentElement*
    /// every frame, never matching anything in the pointer-keyed
    /// cache) into O(1) measure + paint.
    ///
    /// The typed builder mixes type tags into the FNV-1a hash, so
    /// two ids built from semantically-different inputs can't
    /// collide via string-concatenation accidents — the historical
    /// `std::string cache_id` field was removed because that exact
    /// failure mode (forgotten separator, stale generation, recycled
    /// shared_ptr identity) caused observable cell corruption.
    ///
    /// Collisions across unrelated components map them to the same
    /// cache slot — be careful that your id uniquely identifies the
    /// rendered content. Empty means "use pointer keying."
    CacheId hash_id;
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

    /// Optional paint write-back sink (Witness Chain — Trim Accounting).
    /// When non-null, the renderer's paint pass records each item's
    /// laid-out height into the ledger every frame: same layout pass,
    /// same width, same frame as the bytes that reach the wire. This is
    /// what lets ScrollbackLedger mint trim-commit counts from maya's
    /// OWN measurements instead of a host-side re-measure pipeline (the
    /// historical drift source for every trim-corruption bug). The
    /// pointed-to ledger must own `*items_ref` (its elements() vector)
    /// so indices line up; ScrollbackLedger guarantees that pairing
    /// structurally when the host renders through it.
    const ScrollbackLedger* ledger = nullptr;
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
        // The earlier ctor derived the id from sp.get() directly, which
        // collided cache entries across the lifetimes of unrelated
        // shared_ptrs that happened to land at the same heap address.
        // That surfaced as the streaming composer / footer ghost: a
        // cell strip captured for a long-dead shared_ptr blitted under
        // a freshly-allocated shared_ptr at the same address.
        const std::uint64_t cb = detail::id_for_shared(sp);
        c.hash_id = CacheIdBuilder{}
            .add(std::string_view{"shared_ptr<Element>"})
            .add(cb)
            .build();
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
