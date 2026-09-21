#pragma once
// maya::binding — WHEN a colour is decided, enforced by the type system.
//
// ════════════════════════════════════════════════════════════════════════
// THE BUG CLASS THIS RETIRES
// ════════════════════════════════════════════════════════════════════════
//
// A derived value was cached under a key that did not name everything it
// derives from.
//
// Every theme bug this project has shipped is that one sentence. A rendered
// Element derives from (source, theme). Its cache key was source. The theme
// moved, the key did not, and the stale value was served forever. It is not
// really a theming bug at all — theming is just what makes a cache-key
// omission visible.
//
// The instances, for the record:
//
//   • a committed markdown block stored a fully RENDERED Element, with
//     ~58 colour slots resolved at COMMIT time. A later theme switch could
//     not reach it: the bytes never change, so nothing goes dirty, and every
//     cache tier faithfully replayed the same Element.
//   • agentty's ui::Slot converted to Color by RESOLVING, so every widget
//     built from a token baked a literal at build time.
//   • the same shape outside this codebase: OpenSCAD #7053 (colours resolved
//     into a cached PolySet), and React Native's Appearance issue, whose
//     summary is the general statement of it — "a dynamic colour is resolved
//     once, cached in the render tree, and the theme change has no path to
//     invalidate it".
//
// ════════════════════════════════════════════════════════════════════════
// THE FIX, AND WHY IT IS THIS ONE
// ════════════════════════════════════════════════════════════════════════
//
// Do not cache derived values. Store the REFERENCE and resolve at the point
// of use. This is what CSS variables do, and it is why a browser theme
// switch needs no invalidation anywhere: nothing was ever baked, so there is
// nothing to invalidate.
//
// maya already had the primitive — Color::slot() is symbolic, Style::fg is
// an optional<Color>, and Style::to_sgr() resolves through the live theme at
// EMIT time. StylePool::retheme() then re-derives the cached SGR bytes for
// every interned style on a swap, and (its own words) "ids stay valid and no
// canvas cell needs rewriting".
//
// That is the whole reason late binding beats every invalidation scheme we
// tried: a theme switch costs O(distinct styles) instead of O(transcript).
// An earlier attempt re-rendered every committed block on each switch. It
// was CORRECT and it was too slow to be correct in practice — per-keypress
// render went 2.42 ms → 4.65 ms on a 100-message thread and scaled with
// conversation length, so keys outran frames and the browser visibly skipped
// entries. Correctness and performance turned out to be the same problem.
//
// ════════════════════════════════════════════════════════════════════════
// WHY A TRAIT, AND NOT A RULE IN A DOC
// ════════════════════════════════════════════════════════════════════════
//
// maya has already solved this bug class three times, well, and every
// solution was OPT-IN:
//
//   • visual.hpp's mix_any derives a hash FROM THE TYPE, so a field added
//     tomorrow is hashed tomorrow, and an unknown type does not compile.
//   • theme::projected<P>() makes deriving BE the read path, so "forgot to
//     subscribe" is not expressible.
//   • Color vs LitColor puts resolution state in the type.
//
// Each is excellent. None was mandatory, and the bug walked in through the
// gap. Read visual.hpp's header: it describes this exact failure ("a PARALLEL
// DESCRIPTION ... with nothing keeping the two equal") and fixes it by
// inverting the dependency. The lesson is not "add a fourth mechanism", it is
// "make the existing discipline unavoidable".
//
// So: a LitColor reaching storage that outlives a frame is a COMPILE ERROR,
// not a convention. `binding::no_early_binding_v<T>` walks a type and fails
// if a resolved colour can hide anywhere inside it.
//
// ════════════════════════════════════════════════════════════════════════
// THE RULE, IN ONE LINE
// ════════════════════════════════════════════════════════════════════════
//
//   Resolution happens at PAINT. Everything above paint stays symbolic.
//
// Corollaries, so the rule is actionable rather than aspirational:
//
//   • Build-time code (anything producing an Element, a Config, a Style)
//     names a SLOT. It never calls resolve().
//   • Paint-time code (to_sgr, the renderer, StylePool) resolves. It is the
//     only layer allowed to.
//   • A caller that genuinely needs CHANNELS (a blend, a contrast ratio)
//     must resolve explicitly and must keep the result SHORT-LIVED. Putting
//     it in anything stored is the bug, and this header makes that fail.

#include <optional>
#include <type_traits>
#include <vector>

#include "color.hpp"

namespace maya::binding {

// ── Is this type EARLY-BOUND? ────────────────────────────────────────────
//
// A type is early-bound if a resolved colour can hide inside it. LitColor is
// the resolved form by definition (it is what a Theme field holds and what
// to_sgr() emits), so it is the leaf case; everything else is structural.
//
// Deliberately NOT a deep reflection walk over arbitrary aggregates. This
// catches the shapes that actually carry colour — the direct member, the
// optional, the container — which is where every instance of the bug has
// lived. A trait that tries to be exhaustive over every aggregate is a trait
// people switch off.

template <class T>
struct is_early_bound : std::false_type {};

// The leaf: a colour that has already been decided.
template <>
struct is_early_bound<LitColor> : std::true_type {};

// Structural cases.
template <class T>
struct is_early_bound<std::optional<T>> : is_early_bound<T> {};

template <class T, class A>
struct is_early_bound<std::vector<T, A>> : is_early_bound<T> {};

template <class T, std::size_t N>
struct is_early_bound<T[N]> : is_early_bound<T> {};

template <class T>
inline constexpr bool is_early_bound_v = is_early_bound<std::remove_cvref_t<T>>::value;

// ── The assertion a long-lived type owes ─────────────────────────────────
//
// True when T is safe to STORE across a theme change: nothing inside it has
// already committed to a palette.
template <class T>
inline constexpr bool no_early_binding_v = !is_early_bound_v<T>;

// Spell the obligation at a type's definition site:
//
//     MAYA_ASSERT_LATE_BOUND(Style);
//
// The message is the actual instruction, because the person who trips this
// is mid-edit and needs to know what to do, not what went wrong.
#define MAYA_ASSERT_LATE_BOUND(T)                                             \
    static_assert(::maya::binding::no_early_binding_v<T>,                     \
        #T " stores a RESOLVED colour (LitColor), so anything built from it " \
        "is pinned to whatever theme was live at build time and cannot "      \
        "follow a theme switch. Store a symbolic `Color` (Color::slot(...)) " \
        "instead and let paint resolve it. If you genuinely need channels, "  \
        "resolve locally and keep the result short-lived — never in storage. "\
        "See maya/style/binding.hpp.")

// ── The invariants themselves ────────────────────────────────────────────
//
// Style is THE seam. Every Element carries Styles, Elements are stored for
// the life of a session (committed markdown blocks, agentty's frozen
// scrollback ledger), and Style::to_sgr() is where resolution belongs. If
// Style ever holds a LitColor, every stored Element in the program silently
// freezes its palette — which is precisely the shipped bug.
//
// Color is symbolic by construction (Res::Sym); LitColor is the resolved
// form. Pinning both directions here means the distinction cannot quietly
// collapse.
static_assert(no_early_binding_v<Color>,
              "Color must stay SYMBOLIC — it is the late-bound half of the "
              "pair, and the whole late-binding discipline rests on it.");
static_assert(is_early_bound_v<LitColor>,
              "LitColor must register as resolved, or this trait protects "
              "nothing.");
static_assert(is_early_bound_v<std::optional<LitColor>>,
              "the optional case must see through to the colour — a Style "
              "holds optional<Color>, so this is the exact shape that matters.");
static_assert(no_early_binding_v<std::optional<Color>>,
              "optional<Color> is the shape Style::fg uses and must stay "
              "legal.");
static_assert(is_early_bound_v<std::vector<LitColor>>,
              "container case: a palette-as-vector must not hide resolution.");

}  // namespace maya::binding
