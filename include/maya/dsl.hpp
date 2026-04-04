#pragma once
// maya::dsl — Compile-time UI DSL with type-state safety
//
// Define UI trees at compile time using C++26 template metaprogramming.
// Tree structure, text content, styles, and layout properties are all
// resolved at compile time. .build() converts to runtime Element.
//
// Type-state machines enforce correctness at compile time:
//   - Border color requires a border style first
//   - Padding/gap/grow values validated at compile time
//   - Text nodes accept only style modifiers (not layout)
//   - Box nodes accept both style and layout modifiers
//
// Usage:
//   using namespace maya::dsl;
//
//   constexpr auto ui = v(
//       t<"Hello World"> | Bold | Fg<100, 180, 255>,
//       h(
//           t<"Status:"> | Dim,
//           t<"Online"> | Bold | Fg<80, 220, 120>
//       ) | border_<Round> | bcol<50, 55, 70> | pad<1>
//   );
//
//   maya::print(ui.build());
//
// Dynamic content (runtime text returns a proper Node — pipeable):
//   auto ui = v(
//       t<"Live data:"> | Bold,
//       text("count = " + std::to_string(n)) | Fg<100, 255, 180>
//   );

#include <array>
#include <charconv>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "element/builder.hpp"
#include "style/border.hpp"
#include "style/color.hpp"
#include "style/style.hpp"

namespace maya::dsl {

// ── FixedString — string literal as NTTP ─────────────────────────────────────

template <std::size_t N>
struct Str {
    char data[N]{};
    consteval Str(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    consteval std::size_t size() const { return N - 1; }
    constexpr operator std::string_view() const { return {data, N - 1}; }
};

// ── Compile-time style (structural — safe as NTTP) ───────────────────────────

struct CTStyle {
    bool has_fg = false, has_bg = false;
    uint8_t fg_r = 0, fg_g = 0, fg_b = 0;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    bool bold_ = false, dim_ = false, italic_ = false;
    bool underline_ = false, strike_ = false, inverse_ = false;

    consteval CTStyle merge(CTStyle o) const {
        auto s = *this;
        if (o.has_fg) { s.has_fg = true; s.fg_r = o.fg_r; s.fg_g = o.fg_g; s.fg_b = o.fg_b; }
        if (o.has_bg) { s.has_bg = true; s.bg_r = o.bg_r; s.bg_g = o.bg_g; s.bg_b = o.bg_b; }
        s.bold_ |= o.bold_; s.dim_ |= o.dim_; s.italic_ |= o.italic_;
        s.underline_ |= o.underline_; s.strike_ |= o.strike_; s.inverse_ |= o.inverse_;
        return s;
    }

    [[nodiscard]] Style runtime() const {
        Style s;
        if (has_fg) s = s.with_fg(Color::rgb(fg_r, fg_g, fg_b));
        if (has_bg) s = s.with_bg(Color::rgb(bg_r, bg_g, bg_b));
        if (bold_)      s = s.with_bold();
        if (dim_)       s = s.with_dim();
        if (italic_)    s = s.with_italic();
        if (underline_) s = s.with_underline();
        if (strike_)    s = s.with_strikethrough();
        if (inverse_)   s = s.with_inverse();
        return s;
    }
};

// ── Style tag — carries a CTStyle as NTTP ────────────────────────────────────

template <CTStyle V>
struct StyTag {
    static constexpr CTStyle value = V;
};

// ── Style constants ──────────────────────────────────────────────────────────

inline constexpr StyTag<CTStyle{.bold_ = true}>      Bold{};
inline constexpr StyTag<CTStyle{.dim_ = true}>       Dim{};
inline constexpr StyTag<CTStyle{.italic_ = true}>    Italic{};
inline constexpr StyTag<CTStyle{.underline_ = true}> Underline{};
inline constexpr StyTag<CTStyle{.strike_ = true}>    Strike{};
inline constexpr StyTag<CTStyle{.inverse_ = true}>   Inverse{};

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<CTStyle{.has_fg=true, .fg_r=R, .fg_g=G, .fg_b=B}> Fg{};

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<CTStyle{.has_bg=true, .bg_r=R, .bg_g=G, .bg_b=B}> Bg{};

// ── Box layout config (structural NTTP) ──────────────────────────────────────

struct BoxCfg {
    int pad_t = 0, pad_r = 0, pad_b = 0, pad_l = 0;
    int gap = 0, grow = 0;
    BorderStyle bstyle = BorderStyle::None;
    bool has_border = false, has_border_color = false;
    uint8_t bc_r = 0, bc_g = 0, bc_b = 0;
    CTStyle style{};
};

// ── Node concept ─────────────────────────────────────────────────────────────

template <typename T>
concept Node = requires(const T& n) {
    { n.build() } -> std::convertible_to<Element>;
};

/// DslChild: anything that can be a child of v() / h().
/// Either a Node (has .build()) or a range of Elements (vector<Element>, etc.)
template <typename T>
concept DslChild = Node<T> || ElementRange<T>;

// ── Compile-time text node ──────────────────────────────────────────────────

template <Str S, CTStyle Sty = CTStyle{}>
struct TextNode {
    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return Element{TextElement{
            .content = std::string{std::string_view{S}},
            .style   = Sty.runtime(),
        }};
    }
};

// ── Runtime text node — template over content type, pipeable ────────────────
//
// text("hello") | Bold | Fg<255, 100, 80>
//
// Returns RuntimeTextNode<S> which is a proper Node. Style pipes compose at
// the node level — .build() materializes only when the tree is finalized.

template <typename S>
struct RuntimeTextNode {
    S content;
    Style style{};
    TextWrap wrap{TextWrap::Wrap};

    /// Implicit conversion to Element — allows RuntimeTextNode to be used
    /// anywhere an Element is expected (dyn() lambdas, push_back, print()).
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if constexpr (std::integral<S> && !std::same_as<S, bool>) {
            std::array<char, 32> buf;
            auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), content);
            return Element{TextElement{
                .content = std::string{buf.data(), ptr},
                .style   = style,
                .wrap    = wrap,
            }};
        } else if constexpr (std::floating_point<S>) {
            std::array<char, 32> buf;
            auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), content,
                                            std::chars_format::fixed, 2);
            return Element{TextElement{
                .content = std::string{buf.data(), ptr},
                .style   = style,
                .wrap    = wrap,
            }};
        } else {
            return Element{TextElement{
                .content = std::string{std::string_view{content}},
                .style   = style,
                .wrap    = wrap,
            }};
        }
    }
};

// Deduction guides
RuntimeTextNode(const char*) -> RuntimeTextNode<std::string_view>;
RuntimeTextNode(std::string_view) -> RuntimeTextNode<std::string_view>;
RuntimeTextNode(std::string) -> RuntimeTextNode<std::string>;
RuntimeTextNode(int) -> RuntimeTextNode<int>;
RuntimeTextNode(double) -> RuntimeTextNode<double>;

/// Runtime text factory — returns a pipeable Node, not a raw Element.
///   text("hello") | Bold | Fg<255, 100, 80>
///   text(42) | Dim
///   text(3.14)
template <typename S>
[[nodiscard]] auto text(S&& content, Style s = {}) {
    return RuntimeTextNode<std::decay_t<S>>{std::forward<S>(content), s};
}

/// Overload with explicit TextWrap.
template <typename S>
[[nodiscard]] auto text(S&& content, Style s, TextWrap w) {
    return RuntimeTextNode<std::decay_t<S>>{std::forward<S>(content), s, w};
}

// ── Dynamic node — runtime escape hatch in a compile-time tree ───────────────

template <typename F>
struct DynNode {
    F fn;
    operator Element() const { return build(); }
    [[nodiscard]] Element build() const { return fn(); }
};

template <typename F>
    requires std::invocable<F> && std::convertible_to<std::invoke_result_t<F>, Element>
auto dyn(F&& fn) { return DynNode<std::decay_t<F>>{std::forward<F>(fn)}; }

// ── Spacer / Separator / Blank — compile-time nodes ─────────────────────────

struct SpacerNode {
    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return Element{BoxElement{.layout = {.grow = 1.0f}}};
    }
};
inline constexpr SpacerNode space{};

struct SepNode {
    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return maya::detail::box()
            .border(BorderStyle::Single)
            .border_sides(BorderSides::horizontal());
    }
};
inline constexpr SepNode sep{};

struct VSepNode {
    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return maya::detail::box()
            .border(BorderStyle::Single)
            .border_sides(BorderSides::vertical());
    }
};
inline constexpr VSepNode vsep{};

struct BlankNode {
    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return Element{TextElement{.content = ""}};
    }
};
inline constexpr BlankNode blank_{};

/// Function aliases — return nodes, not raw Elements.
[[nodiscard]] constexpr auto spacer()    { return SpacerNode{}; }
[[nodiscard]] constexpr auto separator() { return SepNode{}; }
[[nodiscard]] constexpr auto blank()     { return BlankNode{}; }

// ── Map node — project a runtime range into a Node ──────────────────────────
//
// map(items, [](const auto& s) { return text(s); })
//
// Returns MapNode<R, Proj> which satisfies Node. The range is captured and
// the projection applied lazily in .build().

template <typename R, typename Proj>
struct MapNode {
    R range;
    Proj proj;

    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        std::vector<Element> items;
        if constexpr (std::ranges::sized_range<R>) {
            items.reserve(std::ranges::size(range));
        }
        for (auto&& val : range) {
            if constexpr (Node<std::invoke_result_t<Proj, decltype(val)>>) {
                items.emplace_back(proj(std::forward<decltype(val)>(val)).build());
            } else {
                items.emplace_back(proj(std::forward<decltype(val)>(val)));
            }
        }
        return Element{ElementList{std::move(items)}};
    }
};

/// Map a range through a projection into a Node.
///   map(items, [](const auto& s) { return text(s) | Bold; })
template <std::ranges::range R, typename Proj>
[[nodiscard]] auto map(R&& range, Proj&& proj) {
    return MapNode<std::decay_t<R>, std::decay_t<Proj>>{
        std::forward<R>(range), std::forward<Proj>(proj)};
}

// ── Box node ─────────────────────────────────────────────────────────────────

template <FlexDirection Dir, BoxCfg Cfg, typename... Children>
struct BoxNode {
    std::tuple<Children...> children;

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return std::apply([](const auto&... cs) {
            auto b = maya::detail::box().direction(Dir);
            if constexpr (Cfg.pad_t || Cfg.pad_r || Cfg.pad_b || Cfg.pad_l)
                b = std::move(b).padding(Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l);
            if constexpr (Cfg.gap > 0)
                b = std::move(b).gap(Cfg.gap);
            if constexpr (Cfg.grow > 0)
                b = std::move(b).grow(static_cast<float>(Cfg.grow));
            if constexpr (Cfg.has_border)
                b = std::move(b).border(Cfg.bstyle);
            if constexpr (Cfg.has_border_color)
                b = std::move(b).border_color(Color::rgb(Cfg.bc_r, Cfg.bc_g, Cfg.bc_b));
            constexpr auto& sty = Cfg.style;
            if constexpr (sty.has_fg || sty.has_bg || sty.bold_ || sty.dim_ ||
                          sty.italic_ || sty.underline_ || sty.strike_ || sty.inverse_)
                b = std::move(b).style(sty.runtime());

            // Fast path: all children are Nodes (compile-time known)
            if constexpr ((Node<std::remove_cvref_t<decltype(cs)>> && ...)) {
                return b(cs.build()...);
            } else {
                // Mixed path: some children may be ElementRanges (vector<Element>, etc.)
                std::vector<Element> elems;
                auto collect = [&elems](const auto& c) {
                    using T = std::remove_cvref_t<decltype(c)>;
                    if constexpr (Node<T>) {
                        elems.push_back(c.build());
                    } else {
                        for (const auto& item : c)
                            elems.push_back(Element{item});
                    }
                };
                (collect(cs), ...);
                return b(std::move(elems));
            }
        }, children);
    }
};

// ── Modifier tags with compile-time validation ──────────────────────────────

template <int T, int R = T, int B = T, int L = R>
struct PadTag {
    static_assert(T >= 0 && R >= 0 && B >= 0 && L >= 0,
        "Padding values must be non-negative");
};

template <int G>
struct GapTag {
    static_assert(G >= 0, "Gap must be non-negative");
};

template <BorderStyle BS>
struct BorderTag {};

template <uint8_t R, uint8_t G, uint8_t B>
struct BColTag {};

template <int G = 1>
struct GrowTag {
    static_assert(G >= 0, "Grow factor must be non-negative");
};

template <int T, int R = T, int B = T, int L = R>  inline constexpr PadTag<T,R,B,L> pad{};
template <int G>                                    inline constexpr GapTag<G>       gap_{};
template <BorderStyle BS>                           inline constexpr BorderTag<BS>   border_{};
template <uint8_t R, uint8_t G, uint8_t B>         inline constexpr BColTag<R,G,B>  bcol{};
template <int G = 1>                                inline constexpr GrowTag<G>      grow_{};

// ── operator| : TextNode | Style (compile-time) ────────────────────────────

template <Str S, CTStyle Sty, CTStyle V>
constexpr auto operator|(TextNode<S, Sty>, StyTag<V>) {
    return TextNode<S, Sty.merge(V)>{};
}

// ── operator| : RuntimeTextNode | Style (compile-time style on runtime text)

template <typename S, CTStyle V>
[[nodiscard]] auto operator|(RuntimeTextNode<S> n, StyTag<V>) {
    n.style = n.style.merge(V.runtime());
    return n;
}

// ── operator| : Element | Style (fallback for raw Elements) ─────────────────

template <CTStyle V>
[[nodiscard]] inline Element operator|(Element e, StyTag<V>) {
    return std::move(e) | V.runtime();
}

// ── operator| : Box | Style ─────────────────────────────────────────────────

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, CTStyle V>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, StyTag<V>) {
    constexpr BoxCfg nc = {
        Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b,
        Cfg.style.merge(V)
    };
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// ── operator| : Box | layout modifiers ──────────────────────────────────────

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int T, int R, int B, int L>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, PadTag<T, R, B, L>) {
    constexpr BoxCfg nc = {T, R, B, L, Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int G>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, GapTag<G>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        G, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, BorderStyle BS>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, BorderTag<BS>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, BS, true,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// TYPE-STATE: border color requires border — applying bcol without border is
// a compile error. The requires clause enforces the state machine transition.
template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, uint8_t R, uint8_t G, uint8_t B>
    requires (Cfg.has_border)
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, BColTag<R, G, B>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        true, R, G, B, Cfg.style};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int G>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, GrowTag<G>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, G, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// ── Factory: t<"...">, v(...), h(...) ───────────────────────────────────────

/// Compile-time text: t<"Hello"> | Bold | Fg<255, 100, 80>
template <Str S>
inline constexpr TextNode<S> t{};

/// VStack: v(child1, child2, ...)
/// Accepts any mix of compile-time nodes, runtime Elements, and
/// ranges of Elements (e.g. std::vector<Element>).
template <DslChild... Cs>
constexpr auto v(Cs... cs) {
    return BoxNode<FlexDirection::Column, BoxCfg{}, Cs...>{{cs...}};
}

/// HStack: h(child1, child2, ...)
/// Accepts any mix of compile-time nodes, runtime Elements, and
/// ranges of Elements (e.g. std::vector<Element>).
template <DslChild... Cs>
constexpr auto h(Cs... cs) {
    return BoxNode<FlexDirection::Row, BoxCfg{}, Cs...>{{cs...}};
}

// ── BorderStyle aliases ─────────────────────────────────────────────────────

inline constexpr BorderStyle Round  = BorderStyle::Round;
inline constexpr BorderStyle Single = BorderStyle::Single;
inline constexpr BorderStyle Thick  = BorderStyle::Bold;
inline constexpr BorderStyle Double = BorderStyle::Double;

// ── Runtime builders (promoted from detail) ─────────────────────────────────
//
// For containers that need runtime-configured borders, colors, or border text,
// use these fluent builders alongside the compile-time DSL:
//
//   vstack().border(Round).border_color(theme_color())
//       .border_text(spin() + " CPU", BorderTextPos::Top)
//       .padding(0, 1, 0, 1)(rows)

using maya::detail::vstack;
using maya::detail::hstack;
using maya::detail::center;

// ── Compile-time validation ─────────────────────────────────────────────────

static_assert(Node<TextNode<Str{"hello"}>>,              "TextNode must satisfy Node");
static_assert(Node<RuntimeTextNode<std::string_view>>,   "RuntimeTextNode must satisfy Node");
static_assert(Node<RuntimeTextNode<int>>,                "RuntimeTextNode<int> must satisfy Node");
static_assert(Node<SpacerNode>,                          "SpacerNode must satisfy Node");
static_assert(Node<SepNode>,                             "SepNode must satisfy Node");
static_assert(Node<VSepNode>,                            "VSepNode must satisfy Node");
static_assert(Node<BlankNode>,                           "BlankNode must satisfy Node");

} // namespace maya::dsl
