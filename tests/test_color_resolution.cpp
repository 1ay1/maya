// The resolution index, as an executable specification.
//
// maya::Color is a SUM of literals and a free variable (Kind::Slot), and the
// variable is only meaningful once a Theme substitutes for it. Before this
// split, every consumer had to remember to substitute, and the ones that
// forgot each invented a different wrong answer:
//
//     to_rgb()    on a slot -> white        (the welcome-screen dark slab)
//     put_color() on a slot -> zero bytes   (grid-stream wire desync)
//     degrade()   on a slot -> passthrough  (a slot enum as a red channel)
//
// Now the state is a type parameter: Color = BasicColor<Res::Sym> may hold a
// slot, LitColor = BasicColor<Res::Lit> cannot, and Theme::resolve is the only
// road from the first to the second.
//
// Most of what follows is checked by the compiler and would not link if it
// were false. That is the point — these cases exist so the GUARANTEE is
// named and greppable, and so deleting a `requires` clause in color.hpp
// fails here rather than three months later on someone's light terminal.

#include "agtest.hpp"

#include <maya/style/color.hpp>
#include <maya/style/theme.hpp>
#include <maya/style/schemes.hpp>

#include <print>
#include <string>
#include <type_traits>

using namespace maya;

namespace {

// color.hpp already exports the probes it asserts against (CanNameSlot,
// CanReadChannels, CanProjectRgb, CanDegrade, CanEmitSgr) — reuse them rather
// than restate them, so this file and the header cannot drift apart about
// what the guarantee IS.
//
// They are written against a dependent T on purpose: a requires-expression
// naming a CONSTRAINED STATIC member of a concrete type is not a SFINAE
// context (GCC hard-errors instead of yielding false), so the probe has to be
// a template the compiler substitutes into.
template <class T> concept CanLighten = requires (T c) { c.lighten(0.5f); };

}  // namespace

TEST_CASE("colour: the index is free at runtime") {
    std::println("--- test_color_resolution_phantom ---");
    // A phantom parameter: same bits, same triviality, no vtable, no tag.
    static_assert(sizeof(Color) == sizeof(LitColor));
    static_assert(sizeof(Color) == 4);
    static_assert(std::is_trivially_copyable_v<Color>);
    static_assert(std::is_trivially_copyable_v<LitColor>);
    CHECK(sizeof(Color) == 4);
    std::println("PASS\n");
}

TEST_CASE("colour: Lit is a subtype of Sym, and narrowing is not implicit") {
    std::println("--- test_color_resolution_subtyping ---");
    // Every literal IS a valid symbolic colour, so widening is implicit and
    // free -- which is what let ~1000 existing call sites keep compiling.
    static_assert(std::is_convertible_v<LitColor, Color>);
    // The reverse would be a lie: a Color may hold a slot.
    static_assert(!std::is_convertible_v<Color, LitColor>);

    // Factories are literal BY CONSTRUCTION, whichever index names them.
    static_assert(std::is_same_v<decltype(Color::rgb(1, 2, 3)), LitColor>);
    static_assert(std::is_same_v<decltype(Color::red()), LitColor>);
    static_assert(std::is_same_v<decltype(Color::default_color()), LitColor>);

    const Color widened = Color::rgb(4, 5, 6);   // implicit Lit -> Sym
    CHECK(widened.kind() == ColorKind::Rgb);
    std::println("PASS\n");
}

TEST_CASE("colour: a slot is nameable only where it is meaningful") {
    std::println("--- test_color_resolution_slot_only_on_sym ---");
    static_assert(CanNameSlot<Color>);
    // LitColor::slot(...) does not compile. A literal cannot name a variable.
    static_assert(!CanNameSlot<LitColor>);
    CHECK(Color::slot(ThemeSlot::Accent).kind() == ColorKind::Slot);
    std::println("PASS\n");
}

TEST_CASE("colour: channels are unreachable until resolved") {
    std::println("--- test_color_resolution_channels_need_lit ---");
    // Each of these was a shipped bug. None of them compiles now.
    static_assert(!CanReadChannels<Color>);
    static_assert(!CanProjectRgb<Color>);
    static_assert(!CanDegrade<Color>);
    static_assert(!CanLighten<Color>);
    static_assert(!CanEmitSgr<Color>);

    // ...and all of them are available once the theme has spoken.
    static_assert(CanReadChannels<LitColor>);
    static_assert(CanProjectRgb<LitColor>);
    static_assert(CanDegrade<LitColor>);
    static_assert(CanLighten<LitColor>);
    static_assert(CanEmitSgr<LitColor>);
    std::println("PASS\n");
}

TEST_CASE("colour: resolve is total") {
    std::println("--- test_color_resolution_total ---");
    // resolve() returns LitColor, so a slot cannot survive it. This used to
    // be false in a way no test caught: a Theme field was a Color, so
    // `t.primary = Color::slot(Accent)` made resolve() hand back Kind::Slot,
    // and the slot's enum travelled onward as a red channel. A Theme field is
    // a LitColor now, so that assignment does not compile and the cycle is
    // unrepresentable.
    static_assert(std::is_same_v<decltype(theme::native.resolve(Color{})), LitColor>);
    static_assert(std::is_same_v<decltype(Theme{}.primary), LitColor>);

    // Exhaustive over the enum and over every built-in scheme: no input
    // produces a slot on the far side.
    int checked = 0;
    for (const auto& s : theme::schemes) {
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ThemeSlot::Overlay) + 1; ++i) {
            const LitColor out = s.theme->resolve(Color::slot(static_cast<ThemeSlot>(i)));
            CHECK(out.kind() != ColorKind::Slot);
            ++checked;
        }
    }
    // Guard against the loop silently not running.
    CHECK(checked > 100);
    std::println("PASS ({} slot/scheme pairs resolved)\n", checked);
}

TEST_CASE("colour: a literal passes through resolve untouched") {
    std::println("--- test_color_resolution_literal_passthrough ---");
    // A host override must always beat a widget's slot default, so resolve()
    // is the identity on anything already literal.
    const Theme& t = theme::dracula;
    CHECK(t.resolve(Color::rgb(1, 2, 3)) == Color::rgb(1, 2, 3));
    CHECK(t.resolve(Color::red()) == Color::red());
    CHECK(t.resolve(Color::default_color()) == Color::default_color());
    // ...and a slot does NOT pass through: it becomes the theme's ink.
    CHECK(t.resolve(Color::slot(ThemeSlot::Error)) == t.error);
    std::println("PASS\n");
}

TEST_CASE("colour: try_literal is the one checked way down") {
    std::println("--- test_color_resolution_try_literal ---");
    // Total, and honest about the failure: a slot has no literal meaning, so
    // it yields nullopt rather than a plausible wrong colour.
    CHECK(LitColor::try_literal(Color::rgb(7, 8, 9)).has_value());
    CHECK(LitColor::try_literal(Color::default_color()).has_value());
    CHECK(!LitColor::try_literal(Color::slot(ThemeSlot::Muted)).has_value());
    std::println("PASS\n");
}

TEST_CASE("colour: raw_* reads payload without pretending it is colour") {
    std::println("--- test_color_resolution_raw_bytes ---");
    // Cache keys and serializers need the AUTHORED bytes, not the painted
    // ones: two distinct slots must not collide in a cache just because
    // today's theme happens to paint them alike. raw_* is index-independent
    // and deliberately ugly, so it reads as "I know these are not channels".
    const Color a = Color::slot(ThemeSlot::Accent);
    const Color m = Color::slot(ThemeSlot::Muted);
    CHECK(a.raw_r() != m.raw_r());
    CHECK(Color::rgb(3, 4, 5).raw_g() == 4);
    std::println("PASS\n");
}

TEST_CASE("theme: an unstated slot is a nameable state, not a plausible colour") {
    std::println("--- test_color_resolution_unset ---");
    // A Theme is an aggregate, so a designated initializer that omits a field
    // is legal and value-initializes it. Add a slot to MAYA_THEME_SLOTS and
    // all 57 schemes silently acquire an unstated colour for it.
    //
    // That used to be indistinguishable from a deliberate choice, because a
    // default-constructed Color was Named(7) — white. Well-formed, invisible
    // to every test, and unreadable on a light scheme. ColorKind::Unset makes
    // the omission a state the type can report.
    constexpr Color fresh{};
    static_assert(!fresh.is_set());
    static_assert(fresh.kind() == ColorKind::Unset);
    // Crucially NOT equal to any colour someone might have meant.
    static_assert(!(fresh == Color::white()));
    static_assert(!(fresh == Color::default_color()));

    // It paints as inherit — the terminal's own ink — so the failure mode is
    // "looks unstyled", never "maya invented a colour".
    CHECK(LitColor{}.fg_sgr() == "39");
    CHECK(LitColor{}.bg_sgr() == "49");
    std::println("PASS\n");
}

TEST_CASE("theme: a partial theme is rejected, and it names the slot") {
    std::println("--- test_color_resolution_completeness ---");
    // This is what every scheme looks like the moment someone adds a slot
    // and does not fill it in.
    constexpr Theme partial{ .primary = Color::rgb(1, 2, 3) };
    static_assert(!partial.complete());
    static_assert(partial.first_unset().has_value());
    // Reporting the SLOT rather than a bool is what makes the build failure
    // actionable instead of a puzzle.
    CHECK(slot_field_name(*partial.first_unset()) == "secondary");

    // And every built-in scheme is total. schemes.hpp static_asserts this
    // too — asserted here as well so the guarantee is visible from the test
    // suite, not only as a build error in a header nobody opens.
    int checked = 0;
    for (const auto& s : theme::schemes) {
        CHECK(s.theme->complete());
        ++checked;
    }
    CHECK(checked > 50);
    CHECK(theme::native.complete());
    std::println("PASS ({} schemes total)\n", checked);
}

TEST_CASE("theme: slot metadata is generated from the one list") {
    std::println("--- test_color_resolution_slot_list ---");
    // The enum, the struct fields, resolve()'s switch and this name table all
    // come from MAYA_THEME_SLOTS. If they could drift, adding a slot would
    // fail in three different ways; because they cannot, this just confirms
    // the generated view lines up with the enum.
    static_assert(kThemeSlotCount == 23);
    CHECK(slot_field_name(ThemeSlot::Primary) == "primary");
    CHECK(slot_field_name(ThemeSlot::InverseText) == "inverse_text");
    CHECK(slot_field_name(ThemeSlot::Overlay) == "overlay");

    // Every enumerator has a name and resolves to a set colour under a
    // complete theme — no gaps anywhere in the range.
    for (std::uint8_t i = 0; i < kThemeSlotCount; ++i) {
        const auto s = static_cast<ThemeSlot>(i);
        CHECK(slot_field_name(s) != "?");
        CHECK(theme::native.resolve(Color::slot(s)).is_set());
    }
    std::println("PASS\n");
}

TEST_CASE("colour: is_muted asks on the far side of the theme") {
    std::println("--- test_color_resolution_is_muted ---");
    // Six widgets had each grown a private check against literal
    // bright_black, which silently stopped being true for every scheme whose
    // muted slot is something else. One definition, resolved first.
    CHECK(theme::is_muted(Color::slot(ThemeSlot::Muted)));
    CHECK(!theme::is_muted(Color::slot(ThemeSlot::Accent)));

    // Under a scheme with literal RGB, the muted LITERAL is recognised too --
    // not just the slot that names it.
    theme::set_live(theme::dracula);
    CHECK(theme::is_muted(theme::dracula.muted));
    CHECK(!theme::is_muted(theme::dracula.accent));
    theme::set_live(theme::native);
    std::println("PASS\n");
}
