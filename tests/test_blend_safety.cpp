// blend_safety_test — arithmetic never runs on a colour that has no numbers.
//
// ── The bug this exists to prevent ──────────────────────────────────────
//
// A LitColor is PAINTABLE, not necessarily NUMERIC. Four of its five kinds
// carry no channels:
//
//     Rgb        r/g/b are channels           — arithmetic is meaningful
//     Named      r is a PALETTE INDEX, g/b 0  — arithmetic is nonsense
//     Indexed    r is a PALETTE INDEX, g/b 0  — arithmetic is nonsense
//     Default    nothing at all (SGR 39/49)   — arithmetic is nonsense
//     Unset      nothing at all               — arithmetic is nonsense
//
// The Res::Sym/Res::Lit split made `Slot -> channels` unrepresentable. It
// did NOT close Named/Default -> channels, because r()/g()/b() are gated on
// Res::Lit only. So every blend helper happily read a palette index as a
// red channel:
//
//     bright_black -> Named(8) -> lerp -> rgb(8,0,0) -> "38;2;8;0;0"
//
// which is agentty #45: reasoning blocks painted near-black on a black
// terminal, invisible but selectable. It was reported against the reasoning
// widget, but nothing about it was specific to reasoning — it was reachable
// from lerp(), the gradients and the colour springs, i.e. anywhere maya
// fades anything.
//
// ── Why theme::native is the canary ─────────────────────────────────────
//
// `native` is the ONLY built-in theme that states Named/Default slots, and
// it does so deliberately: that is how the user's own palette, already
// contrast-checked by them, reaches the screen. So native is simultaneously
// the theme most likely to break under a blend and the one least likely to
// be noticed breaking, because every developer runs a scheme.
//
// ── The invariant ───────────────────────────────────────────────────────
//
// An effect that cannot be computed must become NO EFFECT, never a computed
// wrong answer. Concretely: a blend with a channel-less endpoint returns one
// of its endpoints unchanged.
#include <maya/maya.hpp>
#undef NDEBUG
#include "agtest.hpp"

#include <cassert>
#include <print>
#include <string>
#include <vector>

using namespace maya;

namespace {

// Every kind that carries no channels, with a name for the failure message.
struct Channelless { const char* name; LitColor c; };

std::vector<Channelless> channelless_colors() {
    return {
        {"Default",      LitColor::default_color()},
        {"Named(black)", LitColor::black()},
        {"Named(br_blk)",LitColor::bright_black()},
        {"Named(red)",   LitColor::red()},
        {"Named(white)", LitColor::white()},
        {"Indexed(8)",   LitColor::indexed(8)},
        {"Indexed(240)", LitColor::indexed(240)},
    };
}

// An endpoint is "preserved" when the blend handed back one of its inputs
// verbatim — kind AND payload. Comparing the emitted SGR is the honest
// check: that is the byte sequence the terminal actually receives.
bool same_color(LitColor a, LitColor b) {
    return a.kind() == b.kind() && a.fg_sgr() == b.fg_sgr();
}

} // namespace

TEST_CASE("blend safety: has_channels is exactly the Rgb kind") {
    CHECK(LitColor::rgb(1, 2, 3).has_channels());
    CHECK(LitColor::hex(0x282A36).has_channels());
    for (const auto& [name, c] : channelless_colors())
        CHECK(!c.has_channels(), "%s must report no channels", name);
    CHECK(!LitColor{}.has_channels(), "a default-constructed colour has none");
}

TEST_CASE("blend safety: lerp never fabricates channels") {
    // The exact #45 reproduction, as a value: bright_black must never
    // become rgb(8,0,0) — nor anything else with an r of 8 and no g/b.
    const LitColor muted = LitColor::bright_black();
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const LitColor got = anim::lerp(muted, muted, t);
        CHECK(got.kind() != ColorKind::Rgb,
              "lerp(bright_black, bright_black, %.2f) fabricated an Rgb triple "
              "— this is agentty #45", t);
        CHECK(same_color(got, muted),
              "a flat blend must be the identity, got SGR '%s' want '%s'",
              got.fg_sgr().c_str(), muted.fg_sgr().c_str());
    }

    // Mixed pairs: one channel-less endpoint poisons the blend, so the
    // result must snap to an endpoint rather than interpolate.
    const LitColor rgb = LitColor::rgb(200, 180, 255);
    for (const auto& [name, c] : channelless_colors()) {
        for (double t : {0.0, 0.3, 0.7, 1.0}) {
            const LitColor a = anim::lerp(c, rgb, t);
            CHECK(same_color(a, c) || same_color(a, rgb),
                  "lerp(%s, rgb, %.2f) invented a colour: SGR '%s'",
                  name, t, a.fg_sgr().c_str());
            const LitColor b = anim::lerp(rgb, c, t);
            CHECK(same_color(b, c) || same_color(b, rgb),
                  "lerp(rgb, %s, %.2f) invented a colour: SGR '%s'",
                  name, t, b.fg_sgr().c_str());
        }
    }

    // Two genuine Rgb endpoints must still interpolate — the guard must not
    // have turned every fade into a snap.
    const LitColor mid = anim::lerp(LitColor::rgb(0, 0, 0),
                                    LitColor::rgb(100, 200, 40), 0.5);
    CHECK(mid.kind() == ColorKind::Rgb);
    CHECK(mid.r() == 50 && mid.g() == 100 && mid.b() == 20,
          "real blends must still blend, got rgb(%d,%d,%d)",
          mid.r(), mid.g(), mid.b());
}

TEST_CASE("blend safety: darken/lighten leave channel-less colours alone") {
    for (const auto& [name, c] : channelless_colors()) {
        CHECK(same_color(c.darken(0.45f), c),  "darken mutated %s",  name);
        CHECK(same_color(c.lighten(0.45f), c), "lighten mutated %s", name);
    }
}

TEST_CASE("blend safety: gradients never fabricate channels") {
    for (const auto& [name, c] : channelless_colors()) {
        const Gradient g{{c, LitColor::rgb(200, 180, 255)}};
        for (float t : {0.0f, 0.4f, 0.6f, 1.0f}) {
            const LitColor got = g.at(t);
            CHECK(got.kind() == ColorKind::Rgb || same_color(got, c),
                  "Gradient::at(%.2f) over %s invented a colour: SGR '%s'",
                  t, name, got.fg_sgr().c_str());
        }
    }
}

// ── The end-to-end guard ────────────────────────────────────────────────
//
// The three tests above check the helpers in isolation. This one checks the
// PROPERTY that actually matters to a user, across every slot of the theme
// that is most exposed to it: after resolving and blending, is the result
// still something the terminal will render as the user's own colour?
//
// Under native every slot is Named or Default, so the answer must be that
// no blend of any slot with any other ever yields an Rgb triple. That is a
// stronger and much cheaper statement than a WCAG contrast sweep, and it is
// the exact statement #45 violated.
TEST_CASE("blend safety: theme::native survives every pairwise blend") {
    const Theme& th = theme::native;
    std::vector<std::string> offenders;

    for (std::size_t i = 0; i < kThemeSlotCount; ++i) {
        const auto si = static_cast<ThemeSlot>(i);
        const LitColor a = th.resolve(Color::slot(si));
        const std::string ni{slot_field_name(si)};

        CHECK(!a.has_channels(),
              "native slot '%s' resolved to an Rgb triple — native must state "
              "only Named/Default so the user's palette shows through",
              ni.c_str());

        for (std::size_t j = 0; j < kThemeSlotCount; ++j) {
            const auto sj = static_cast<ThemeSlot>(j);
            const LitColor b = th.resolve(Color::slot(sj));
            for (double t : {0.0, 0.5, 1.0}) {
                const LitColor got = anim::lerp(a, b, t);
                if (got.kind() == ColorKind::Rgb)
                    offenders.push_back(ni + " x "
                                        + std::string(slot_field_name(sj))
                                        + " -> " + got.fg_sgr());
            }
        }
    }

    if (!offenders.empty()) {
        std::println("{} native slot blend(s) fabricated an RGB triple:",
                     offenders.size());
        for (std::size_t i = 0; i < offenders.size() && i < 20; ++i)
            std::println("   {}", offenders[i]);
        std::println("\nA blend against a channel-less colour must return an");
        std::println("endpoint, not arithmetic on a palette index. See");
        std::println("BasicColor::has_channels() and anim::lerp().");
    }
    CHECK(offenders.empty());
}
