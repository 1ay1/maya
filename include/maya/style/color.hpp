#pragma once
// maya::style::color - Compile-time type-safe color system
//
// Every color is constexpr-constructible and validated at compile time.
// Supports ANSI 16, ANSI 256, and 24-bit truecolor. Colors are value types -
// small, trivially copyable, and zero-cost to pass around.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>

namespace maya {

// ============================================================================
// Color - A tagged union of color representations
// ============================================================================
// Like Rust's enum Color { Named(AnsiColor), Indexed(u8), Rgb(u8,u8,u8) }
// but in C++ with constexpr everything.

enum class AnsiColor : uint8_t {
    Black        = 0,
    Red          = 1,
    Green        = 2,
    Yellow       = 3,
    Blue         = 4,
    Magenta      = 5,
    Cyan         = 6,
    White        = 7,
    BrightBlack  = 8,
    BrightRed    = 9,
    BrightGreen  = 10,
    BrightYellow = 11,
    BrightBlue   = 12,
    BrightMagenta= 13,
    BrightCyan   = 14,
    BrightWhite  = 15,
};

namespace detail {

// xterm 256-color 6×6×6 cube levels.
inline constexpr int kCubeLevels[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};

// The 16 standard ANSI colors as RGB, for nearest-color downgrades.
inline constexpr int kAnsi16[16][3] = {
    {0,0,0},   {128,0,0},  {0,128,0},  {128,128,0},
    {0,0,128}, {128,0,128},{0,128,128},{192,192,192},
    {128,128,128},{255,0,0},{0,255,0}, {255,255,0},
    {0,0,255}, {255,0,255},{0,255,255},{255,255,255},
};

[[nodiscard]] constexpr int color_dist2(int r1,int g1,int b1,
                                        int r2,int g2,int b2) noexcept {
    int dr=r1-r2, dg=g1-g2, db=b1-b2;
    return dr*dr + dg*dg + db*db;
}

// Snap one channel to the nearest of the 6 cube levels (index 0..5).
[[nodiscard]] constexpr int cube_index(int v) noexcept {
    if (v < 48) return 0;
    if (v < 115) return 1;
    return (v - 35) / 40;   // 2..5
}

// RGB → nearest xterm-256 index, choosing between the color cube and the
// 24-step grayscale ramp by Euclidean distance.
[[nodiscard]] constexpr uint8_t rgb_to_xterm256(int r,int g,int b) noexcept {
    int ri=cube_index(r), gi=cube_index(g), bi=cube_index(b);
    int cr=kCubeLevels[ri], cg=kCubeLevels[gi], cb=kCubeLevels[bi];
    int cube = 16 + 36*ri + 6*gi + bi;
    int gray = (r*299 + g*587 + b*114) / 1000;             // Rec.601 luma
    int gidx = gray < 8 ? 0 : gray > 238 ? 23 : (gray - 3) / 10;
    int gv = 8 + 10*gidx;
    if (color_dist2(cr,cg,cb, r,g,b) <= color_dist2(gv,gv,gv, r,g,b))
        return static_cast<uint8_t>(cube);
    return static_cast<uint8_t>(232 + gidx);
}

// RGB → nearest of the 16 ANSI colors.
[[nodiscard]] constexpr uint8_t rgb_to_ansi16(int r,int g,int b) noexcept {
    int best=0, bestd=2147483647;
    for (int i=0;i<16;++i) {
        int d = color_dist2(kAnsi16[i][0],kAnsi16[i][1],kAnsi16[i][2], r,g,b);
        if (d < bestd) { bestd=d; best=i; }
    }
    return static_cast<uint8_t>(best);
}

struct Rgb3 { int r, g, b; };

// xterm-256 index → RGB, so an already-indexed color can be re-quantized to 16.
[[nodiscard]] constexpr Rgb3 xterm256_to_rgb(int i) noexcept {
    if (i < 16) return {kAnsi16[i][0], kAnsi16[i][1], kAnsi16[i][2]};
    if (i < 232) {
        int c = i - 16;
        return {kCubeLevels[(c/36)%6], kCubeLevels[(c/6)%6], kCubeLevels[c%6]};
    }
    int v = 8 + 10*(i - 232);
    return {v, v, v};
}

} // namespace detail

// ── Theme slots, as a colour ────────────────────────────────────────────────
//
// A Color can name a SEMANTIC SLOT instead of a literal hue — "accent",
// "muted", "error" — and be resolved against the theme in force when it is
// painted rather than when it is written.
//
// This is what lets a widget's Config carry a sensible default that still
// follows the user's theme. Before this, a field like
//
//     Color accent_color = Color::magenta();
//
// pinned that widget to magenta for the life of the process: it is a default
// on a struct, evaluated once, with no theme in scope to consult. Multiply by
// the ~540 such defaults across maya's widgets and "pick a theme" repainted
// only the handful of colours a host happened to pass explicitly — which is
// exactly the "some things change, most don't" symptom.
//
// Writing it as
//
//     Color accent_color = Color::slot(ThemeSlot::Accent);
//
// keeps the default meaningful, keeps it constexpr, and makes it a question
// answered at paint time. Hosts that pass an explicit colour still win — an
// override is a literal and literals are left alone.
enum class ThemeSlot : uint8_t {
    Primary, Secondary, Accent,
    Success, Error, Warning, Info,
    Text, InverseText, Muted,
    Surface, Background, Border,
    DiffAdded, DiffRemoved, DiffChanged,
    Highlight, Selection, Cursor, Link, Placeholder, Shadow, Overlay,
};

// ── The resolution index ────────────────────────────────────────────────────
//
// `Slot` is not a colour. It is a FREE VARIABLE standing for one, and the
// theme is the environment that substitutes for it. Keeping both in one
// type made every consumer handle a case it has no answer for, and C++ let
// each pick a different wrong answer silently:
//
//     to_rgb()    on a slot -> white        (the welcome-screen dark slab)
//     put_color() on a slot -> zero bytes   (grid-stream desync)
//     degrade()   on a slot -> passthrough  (a slot index as a colour channel)
//
// So the resolution state is lifted into the TYPE. `Res` indexes the colour
// by whether a slot can still be hiding inside it:
//
//     Color    = BasicColor<Res::Sym>   what widget configs and hosts hold
//     LitColor = BasicColor<Res::Lit>   what the renderer is allowed to eat
//
// Three rules, all enforced by the compiler rather than by review:
//
//   1. slot() exists ONLY on Sym.       A literal cannot name a variable.
//   2. channels exist ONLY on Lit.      A variable has no channels to read.
//   3. Lit converts to Sym implicitly, Sym to Lit NEVER.
//
// Rule 3 is the interesting one: Lit is a SUBTYPE of Sym (every literal is a
// valid symbolic colour), so widening is free and every existing call site
// keeps compiling. The only way back down is Theme::resolve(), which is
// therefore the single introduction rule for a paintable colour — and, being
// the only one, cannot be forgotten.
enum class Res : bool { Sym, Lit };

// The colour's shape. Namespace-scope rather than nested in BasicColor: the
// kind is a property of the COLOUR, not of its resolution state, and nesting
// it would give Color::Kind::Rgb and LitColor::Kind::Rgb as two unrelated
// types that will not compare.
//
// `Unset` is the DEFAULT, and it is what a value-initialized colour is. It
// exists so "nobody filled this in" is a state the type can tell you about,
// instead of a state that looks exactly like a deliberate choice.
//
// Before it, BasicColor() was Named(7) — white. That is a real colour a
// scheme might mean, so a FORGOTTEN Theme field and an intentional white
// were the same bits, and nothing could distinguish them. Add a slot to
// Theme and all 57 designated-initializer schemes silently acquire a
// hardcoded white for it: unreadable on a light scheme, and invisible to
// every test, because the value is well-formed. Unset makes that
// omission nameable, which is what lets Theme::complete() reject it at
// compile time.
enum class ColorKind : uint8_t { Unset, Named, Indexed, Rgb, Default, Slot };

template <Res R> class BasicColor;
using Color    = BasicColor<Res::Sym>;
using LitColor = BasicColor<Res::Lit>;

template <Res R>
class BasicColor {
public:
    using Kind = ColorKind;   // compatibility spelling: Color::Kind::Rgb

private:
    Kind    kind_;
    uint8_t r_, g_, b_;  // For Rgb; r_ doubles as index for Named/Indexed

    constexpr BasicColor(Kind k, uint8_t r, uint8_t g, uint8_t b) noexcept
        : kind_(k), r_(r), g_(g), b_(b) {}

    // The two indices are the same bits; each may read the other's.
    template <Res> friend class BasicColor;

public:
    // Constructors - all constexpr
    //
    // Value-initializes to UNSET, not to white. See ColorKind::Unset: a
    // default-constructed colour is "nobody said", and saying so is what
    // makes a forgotten Theme slot a compile error rather than a plausible
    // wrong colour. Anything that paints treats Unset as the terminal's own
    // ink (the same answer Default gives), so the failure mode is inherit,
    // never a colour maya invented.
    constexpr BasicColor() noexcept : kind_(Kind::Unset), r_(0), g_(0), b_(0) {}

    /// Was this colour ever given a value?
    [[nodiscard]] constexpr bool is_set() const noexcept { return kind_ != Kind::Unset; }

    constexpr explicit BasicColor(AnsiColor c) noexcept
        : kind_(Kind::Named), r_(static_cast<uint8_t>(c)), g_(0), b_(0) {}

    constexpr BasicColor(const BasicColor&) noexcept = default;
    constexpr BasicColor& operator=(const BasicColor&) noexcept = default;

    /// Widening, Lit -> Sym. A template so it never displaces the copy
    /// constructor (as a plain converting ctor would, making `Lit x = lit;`
    /// itself ill-formed — a trap worth naming, since it costs an afternoon).
    template <Res S> requires (S == Res::Lit && R == Res::Sym)
    constexpr BasicColor(const BasicColor<S>& o) noexcept
        : kind_(o.kind_), r_(o.r_), g_(o.g_), b_(o.b_) {}

    // Named color factories.
    //
    // Every one of these is LITERAL by construction, so they return LitColor
    // whichever index they are named through — `Color::red()` still works and
    // still widens for free. That is what keeps ~1000 existing call sites
    // compiling untouched while the renderer gets a type it can trust.
    //
    // `auto`, NOT `LitColor`, and that is load-bearing rather than style.
    // LitColor IS BasicColor<Res::Lit>, so naming it in a signature inside
    // BasicColor's own definition asks for the type to be complete while it
    // is still being defined — which is exactly what it is not, when R is
    // Lit. GCC and Clang accept it; MSVC rejects it, and did:
    //
    //   error C7637: maya::BasicColor<maya::Res::Lit>: you cannot implicitly
    //   instantiate a class template while it is being defined
    //
    // A deduced return type is resolved at INSTANTIATION, by which point the
    // class is complete, so the self-reference never arises. Same type, same
    // constexpr-ness, no forward-declaration dance.
    static constexpr auto black()          noexcept { return LitColor{AnsiColor::Black}; }
    static constexpr auto red()            noexcept { return LitColor{AnsiColor::Red}; }
    static constexpr auto green()          noexcept { return LitColor{AnsiColor::Green}; }
    static constexpr auto yellow()         noexcept { return LitColor{AnsiColor::Yellow}; }
    static constexpr auto blue()           noexcept { return LitColor{AnsiColor::Blue}; }
    static constexpr auto magenta()        noexcept { return LitColor{AnsiColor::Magenta}; }
    static constexpr auto cyan()           noexcept { return LitColor{AnsiColor::Cyan}; }
    static constexpr auto white()          noexcept { return LitColor{AnsiColor::White}; }
    static constexpr auto bright_black()   noexcept { return LitColor{AnsiColor::BrightBlack}; }
    static constexpr auto bright_red()     noexcept { return LitColor{AnsiColor::BrightRed}; }
    static constexpr auto bright_green()   noexcept { return LitColor{AnsiColor::BrightGreen}; }
    static constexpr auto bright_yellow()  noexcept { return LitColor{AnsiColor::BrightYellow}; }
    static constexpr auto bright_blue()    noexcept { return LitColor{AnsiColor::BrightBlue}; }
    static constexpr auto bright_magenta() noexcept { return LitColor{AnsiColor::BrightMagenta}; }
    static constexpr auto bright_cyan()    noexcept { return LitColor{AnsiColor::BrightCyan}; }
    static constexpr auto bright_white()   noexcept { return LitColor{AnsiColor::BrightWhite}; }
    static constexpr auto gray()           noexcept { return bright_black(); }
    static constexpr auto grey()           noexcept { return bright_black(); }

    /// Terminal-default color (SGR 39 fg / 49 bg). Use this when you want a
    /// container to occlude underlying cells in a zstack while still showing
    /// the user's terminal theme background.
    static constexpr auto default_color() noexcept {
        return LitColor{LitColor::Kind::Default, 0, 0, 0};
    }

    // 256-color palette
    static constexpr auto indexed(uint8_t index) noexcept {
        return LitColor{LitColor::Kind::Indexed, index, 0, 0};
    }

    /// A semantic theme slot, resolved at PAINT time against the theme in
    /// force. See ThemeSlot above for why widget Config defaults use this
    /// instead of a literal.
    ///
    /// Sym-only. A LitColor is by definition one no slot can hide inside, so
    /// `LitColor::slot(...)` is a compile error rather than a colour that
    /// silently paints as white somewhere downstream.
    static constexpr auto slot(ThemeSlot s) noexcept requires (R == Res::Sym) {
        return Color{Color::Kind::Slot, static_cast<uint8_t>(s), 0, 0};
    }

    [[nodiscard]] constexpr ThemeSlot theme_slot() const noexcept {
        return static_cast<ThemeSlot>(r_);
    }

    /// Narrowing, Sym -> Lit, CHECKED.
    ///
    /// The only way down the subtype relation, and it is total: a slot has no
    /// literal meaning, so it yields nullopt rather than a plausible wrong
    /// colour. Theme::resolve() is the one caller that matters — it handles
    /// the slot case itself and uses this for the pass-through — but it is
    /// public because a host that has just tested `kind() != Slot` deserves a
    /// way to say so that does not involve a cast.
    ///
    /// Named `try_` because the failure is a real case, not a contract
    /// violation: asking a variable for its value is a fair question with the
    /// honest answer "not yet".
    ///
    /// Unset narrows fine, and deliberately: "nobody stated a colour" IS a
    /// paintable answer (inherit, SGR 39/49), unlike a slot, which is a
    /// question the theme has to answer first.
    ///
    /// `auto` for the same reason as the factories above, and this one is the
    /// harder case: `std::optional<LitColor>` does not merely NAME the
    /// incomplete type, it instantiates a template over it, so MSVC needs the
    /// full definition to compute optional's storage and triviality traits.
    /// That is why the earlier pass over the factories was not enough -- it
    /// removed the mentions that were easy to see and left the two that
    /// actually reach into <optional> and <xsmf_control>.
    static constexpr auto try_literal(Color c) noexcept
        requires (R == Res::Lit)
    {
        if (c.kind_ == ColorKind::Slot) return std::optional<LitColor>{};
        return std::optional<LitColor>{LitColor{c.kind_, c.r_, c.g_, c.b_}};
    }

    // 24-bit truecolor
    static constexpr auto rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return LitColor{LitColor::Kind::Rgb, r, g, b};
    }

    // From hex literal: Color::hex(0xFF00FF)
    //
    // `auto` rather than LitColor: same self-reference as above.
    static consteval auto hex(uint32_t rgb) {
        if (rgb > 0xFFFFFF) throw "Color::hex: value exceeds 0xFFFFFF";
        return BasicColor::rgb(
            static_cast<uint8_t>((rgb >> 16) & 0xFF),
            static_cast<uint8_t>((rgb >> 8) & 0xFF),
            static_cast<uint8_t>(rgb & 0xFF)
        );
    }

    // HSL to RGB conversion (constexpr)
    static constexpr auto hsl(float h, float s, float l) noexcept {
        // Normalize h to [0, 360)
        while (h < 0) h += 360;
        while (h >= 360) h -= 360;
        s = std::clamp(s, 0.0f, 1.0f);
        l = std::clamp(l, 0.0f, 1.0f);

        auto hue2rgb = [](float p, float q, float t) -> float {
            if (t < 0) t += 1;
            if (t > 1) t -= 1;
            if (t < 1.0f/6) return p + (q - p) * 6 * t;
            if (t < 1.0f/2) return q;
            if (t < 2.0f/3) return p + (q - p) * (2.0f/3 - t) * 6;
            return p;
        };

        float r, g, b;
        if (s == 0) {
            r = g = b = l;
        } else {
            float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
            float p = 2 * l - q;
            float hn = h / 360.0f;
            r = hue2rgb(p, q, hn + 1.0f/3);
            g = hue2rgb(p, q, hn);
            b = hue2rgb(p, q, hn - 1.0f/3);
        }
        return BasicColor::rgb(
            static_cast<uint8_t>(r * 255 + 0.5f),
            static_cast<uint8_t>(g * 255 + 0.5f),
            static_cast<uint8_t>(b * 255 + 0.5f)
        );
    }

    // Accessors
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }

    /// Channel / palette-index reads — Lit only.
    ///
    /// On a slot these bytes are the ThemeSlot enum, not a colour: reading
    /// them is how a slot index used to reach a terminal as a red channel.
    /// Resolve first and the question is well-posed; until then it is not a
    /// question this type can answer, so it does not offer to.
    ///
    /// RESOLVING IS NOT ENOUGH. A LitColor is paintable, not necessarily
    /// NUMERIC: on Named/Indexed these bytes are still a palette index, and
    /// on Default/Unset they are nothing at all. `theme::native` states its
    /// slots as Named and Default precisely so the user's own palette shows
    /// through — so under native, every one of these reads is a lie:
    ///
    ///     bright_black -> Named(8) -> r()=8, g()=0, b()=0 -> rgb(8,0,0)
    ///
    /// which is the invisible-reasoning-block bug (agentty #45): a palette
    /// index painted as a near-black truecolor triple. Ask has_channels()
    /// before doing ARITHMETIC on these, or go through to_rgb(), which maps
    /// the palette properly. Reading them to EMIT a palette index is fine
    /// and correct — that is what index() is for; prefer it there so the
    /// two meanings are never spelled the same way.
    [[nodiscard]] constexpr uint8_t r() const noexcept requires (R == Res::Lit) { return r_; }
    [[nodiscard]] constexpr uint8_t g() const noexcept requires (R == Res::Lit) { return g_; }
    [[nodiscard]] constexpr uint8_t b() const noexcept requires (R == Res::Lit) { return b_; }
    [[nodiscard]] constexpr uint8_t index() const noexcept requires (R == Res::Lit) { return r_; }

    /// True when r()/g()/b() are CHANNELS rather than a palette index or
    /// nothing — i.e. when arithmetic on this colour is meaningful.
    ///
    /// This is the guard every blend must apply. darken()/lighten() have
    /// always had it inline (`if (kind_ != Kind::Rgb) return *this`); naming
    /// it is what let lerp(), the springs and the gradients adopt the same
    /// rule instead of each reinventing the hazard.
    [[nodiscard]] constexpr bool has_channels() const noexcept {
        return kind_ == Kind::Rgb;
    }

    /// Raw payload bytes, index-independent.
    ///
    /// NOT channels: for Named/Indexed this is a palette index, and for a
    /// slot it is the ThemeSlot enum. Reading them as colour is the bug this
    /// file's type split exists to prevent, so they are deliberately ugly to
    /// spell and exist for exactly two jobs:
    ///
    ///   - HASHING a style for cache identity, where the question is "are
    ///     these the same authored value", not "what colour is this". A hash
    ///     that resolved first would collide two different slots that happen
    ///     to share a hue under today's theme.
    ///   - SERIALIZING the value itself.
    ///
    /// If you want to paint it, resolve() instead.
    [[nodiscard]] constexpr uint8_t raw_r() const noexcept { return r_; }
    [[nodiscard]] constexpr uint8_t raw_g() const noexcept { return g_; }
    [[nodiscard]] constexpr uint8_t raw_b() const noexcept { return b_; }

    // Downgrade this color to what a terminal of the given capability `level`
    // can actually display:  3 = truecolor (unchanged), 2 = 256-color,
    // 1 = 16-color.  RGB and 256-indexed colors are mapped to the nearest
    // representable color so apps look right on terminals without 24-bit
    // support — most importantly macOS Terminal.app, which is 256-color only
    // and silently drops `38;2` truecolor escapes. Named/Default colors and
    // anything already within the terminal's range pass through untouched.
    // Color is never stripped here; honoring "no color at all" is the
    // caller's job (it simply omits the SGR).
    //
    // Lit only. The switch below has no Slot case and needs none: on this
    // index the kind cannot be Slot, so -Wswitch proves the function total
    // instead of a `return *this` fallthrough quietly shipping a slot index
    // onward as though it were a colour.
    [[nodiscard]] constexpr LitColor degrade(int level) const noexcept
        requires (R == Res::Lit)
    {
        if (level >= 3) return *this;
        switch (kind_) {
            case Kind::Unset:
            case Kind::Default:
            case Kind::Named:
                return *this;
            case Kind::Indexed: {
                if (level >= 2) return *this;
                detail::Rgb3 c = detail::xterm256_to_rgb(r_);
                return LitColor{static_cast<AnsiColor>(
                    detail::rgb_to_ansi16(c.r, c.g, c.b))};
            }
            case Kind::Rgb:
                if (level >= 2)
                    return BasicColor::indexed(detail::rgb_to_xterm256(r_, g_, b_));
                return LitColor{static_cast<AnsiColor>(
                    detail::rgb_to_ansi16(r_, g_, b_))};
            case Kind::Slot:
                break;  // unreachable on Lit; keeps the switch exhaustive
        }
        return *this;
    }

    // Generate foreground SGR codes
    [[nodiscard]] std::string fg_sgr() const requires (R == Res::Lit) {
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 30 + r_ : 90 + (r_ - 8);
                return std::to_string(code);
            }
            case Kind::Indexed:
                return "38;5;" + std::to_string(r_);
            case Kind::Rgb:
                return "38;2;" + std::to_string(r_) + ";" +
                       std::to_string(g_) + ";" + std::to_string(b_);
            // Unset paints as inherit: nobody stated a colour, so the
            // terminal's own ink is the only honest answer.
            case Kind::Unset:
            case Kind::Default:
                return "39";
            case Kind::Slot:
                break;  // unreachable on Lit
        }
        __builtin_unreachable();
    }

    // Generate background SGR codes
    [[nodiscard]] std::string bg_sgr() const requires (R == Res::Lit) {
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 40 + r_ : 100 + (r_ - 8);
                return std::to_string(code);
            }
            case Kind::Indexed:
                return "48;5;" + std::to_string(r_);
            case Kind::Rgb:
                return "48;2;" + std::to_string(r_) + ";" +
                       std::to_string(g_) + ";" + std::to_string(b_);
            case Kind::Unset:
            case Kind::Default:
                return "49";
            case Kind::Slot:
                break;  // unreachable on Lit
        }
        __builtin_unreachable();
    }

    // -------------------------------------------------------------------------
    // Zero-allocation SGR emitters — write directly into an existing string.
    // Avoids any heap allocation on the hot rendering path.
    // -------------------------------------------------------------------------

    void append_fg_sgr(std::string& out) const requires (R == Res::Lit) {
        char buf[16];
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 30 + r_ : 90 + (r_ - 8);
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), code);
                out.append(buf, p);
                break;
            }
            case Kind::Indexed: {
                out += "38;5;";
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p);
                break;
            }
            case Kind::Rgb: {
                out += "38;2;";
                auto [p1, _1] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p1); out += ';';
                auto [p2, _2] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(g_));
                out.append(buf, p2); out += ';';
                auto [p3, _3] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(b_));
                out.append(buf, p3);
                break;
            }
            case Kind::Unset:
            case Kind::Default:
                out += "39";
                break;
        }
    }

    void append_bg_sgr(std::string& out) const requires (R == Res::Lit) {
        char buf[16];
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 40 + r_ : 100 + (r_ - 8);
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), code);
                out.append(buf, p);
                break;
            }
            case Kind::Indexed: {
                out += "48;5;";
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p);
                break;
            }
            case Kind::Rgb: {
                out += "48;2;";
                auto [p1, _1] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p1); out += ';';
                auto [p2, _2] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(g_));
                out.append(buf, p2); out += ';';
                auto [p3, _3] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(b_));
                out.append(buf, p3);
                break;
            }
            case Kind::Unset:
            case Kind::Default:
                out += "49";
                break;
        }
    }

    // Lighten/darken (returns new color)
    [[nodiscard]] constexpr LitColor lighten(float amount) const noexcept
        requires (R == Res::Lit)
    {
        if (kind_ != Kind::Rgb) return *this;
        auto lift = [amount](uint8_t c) -> uint8_t {
            float v = static_cast<float>(c) / 255.0f;
            v += (1.0f - v) * amount;
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255);
        };
        return BasicColor::rgb(lift(r_), lift(g_), lift(b_));
    }

    [[nodiscard]] constexpr LitColor darken(float amount) const noexcept
        requires (R == Res::Lit)
    {
        if (kind_ != Kind::Rgb) return *this;
        auto drop = [amount](uint8_t c) -> uint8_t {
            float v = static_cast<float>(c) / 255.0f;
            v *= (1.0f - amount);
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255);
        };
        return BasicColor::rgb(drop(r_), drop(g_), drop(b_));
    }

    // Project to concrete Rgb channels. Named goes through the ANSI-16
    // palette, Indexed through the xterm-256 table, Rgb passes through, and
    // Default falls back to white ("assume light ink" — the terminal's real
    // foreground is not knowable from here).
    //
    // Lit only, and that is the whole point: this used to answer `white` for
    // a slot, because a slot's enum lives in the red byte and reading it as
    // an ANSI index returns a plausible, wrong colour. That sentinel shipped
    // as the welcome-screen dark slab. There is nothing to apologise for now
    // — a slot cannot reach this function, so the case is gone rather than
    // guessed at, and the theme-discipline regex that policed it is obsolete.
    [[nodiscard]] constexpr LitColor to_rgb() const noexcept
        requires (R == Res::Lit)
    {
        switch (kind_) {
            case Kind::Rgb:
                return *this;
            case Kind::Named: {
                const auto& c = detail::kAnsi16[r_ & 15];
                return BasicColor::rgb(static_cast<uint8_t>(c[0]),
                                       static_cast<uint8_t>(c[1]),
                                       static_cast<uint8_t>(c[2]));
            }
            case Kind::Indexed: {
                detail::Rgb3 c = detail::xterm256_to_rgb(r_);
                return BasicColor::rgb(static_cast<uint8_t>(c.r),
                                       static_cast<uint8_t>(c.g),
                                       static_cast<uint8_t>(c.b));
            }
            case Kind::Unset:
            case Kind::Default:
                return BasicColor::rgb(255, 255, 255);
            case Kind::Slot:
                break;  // unreachable on Lit
        }
        return *this;
    }

    constexpr auto operator<=>(const BasicColor&) const = default;

    /// Cross-index comparison. A literal and a symbolic colour holding the
    /// same bits ARE the same colour, and refusing to say so would just push
    /// callers back to hand-comparing bytes.
    template <Res S>
    [[nodiscard]] constexpr bool operator==(const BasicColor<S>& o) const noexcept {
        return kind_ == o.kind_ && r_ == o.r_ && g_ == o.g_ && b_ == o.b_;
    }
};

// Compile-time validation
static_assert(Color::hex(0xFF00FF).r() == 255);
static_assert(Color::hex(0xFF00FF).g() == 0);
static_assert(Color::hex(0xFF00FF).b() == 255);
static_assert(Color::red().kind() == Color::Kind::Named);
static_assert(Color::indexed(42).index() == 42);

// ── The resolution index, asserted ──────────────────────────────────────────
//
// The guarantees this file exists to provide, stated as code so they are
// checked on every build rather than believed. The negative half uses
// requires-expressions: each must be FALSE, i.e. the operation must not
// compile. Deleting a `requires (R == Res::Lit)` clause below breaks the
// build here, which is the point.

// Phantom: the index costs nothing at runtime.
static_assert(sizeof(Color) == sizeof(LitColor));
static_assert(sizeof(Color) == 4);
static_assert(std::is_trivially_copyable_v<Color>);
static_assert(std::is_trivially_copyable_v<LitColor>);

// Factories are literal by construction, whichever index names them.
static_assert(std::is_same_v<decltype(Color::rgb(1, 2, 3)), LitColor>);
static_assert(std::is_same_v<decltype(Color::red()), LitColor>);
static_assert(std::is_same_v<decltype(Color::default_color()), LitColor>);

// Lit <: Sym. Widening is implicit; narrowing does not exist.
static_assert(std::is_convertible_v<LitColor, Color>);
static_assert(!std::is_convertible_v<Color, LitColor>);

// A slot is nameable only on the symbolic index...
//
// These probes are written against a dependent `T` on purpose. A
// requires-expression naming a CONSTRAINED STATIC member of a concrete type
// is not a SFINAE context — GCC hard-errors instead of yielding false — so
// the probe has to be a template the compiler substitutes into.
template <class T>
concept CanNameSlot = requires { T::slot(ThemeSlot::Accent); };
template <class T>
concept CanReadChannels = requires (T c) { c.r(); };
template <class T>
concept CanProjectRgb = requires (T c) { c.to_rgb(); };
template <class T>
concept CanDegrade = requires (T c) { c.degrade(3); };
template <class T>
concept CanEmitSgr = requires (T c, std::string& s) { c.append_fg_sgr(s); };

static_assert(CanNameSlot<Color>);
static_assert(!CanNameSlot<LitColor>);

// ...and channels are readable only on the literal one. These are exactly
// the operations that shipped as bugs when handed an unresolved slot.
static_assert(!CanReadChannels<Color>);
static_assert(!CanProjectRgb<Color>);
static_assert(!CanDegrade<Color>);
static_assert(!CanEmitSgr<Color>);
static_assert(CanReadChannels<LitColor>);
static_assert(CanProjectRgb<LitColor>);
static_assert(CanDegrade<LitColor>);
static_assert(CanEmitSgr<LitColor>);

} // namespace maya
