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

// ── Wrap tags ───────────────────────────────────────────────────────────────

struct TruncateTag {};
struct NoWrapTag {};

/// Truncate text with ellipsis when it overflows:  text("long...") | clip
inline constexpr TruncateTag  clip{};

/// Never break text (may overflow container):  text("long...") | nowrap
inline constexpr NoWrapTag nowrap{};

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<CTStyle{.has_fg=true, .fg_r=R, .fg_g=G, .fg_b=B}> Fg{};

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<CTStyle{.has_bg=true, .bg_r=R, .bg_g=G, .bg_b=B}> Bg{};

// ── Hex color shorthand ─────────────────────────────────────────────────────
//
// Hex colors avoid the 3-arg verbosity of Fg<R,G,B>:
//   text("error") | fg<0xFF4444>
//   v(...) | bg<0x1A1A2E>
//
// consteval decomposition validates range at compile time.

namespace detail_color {
    consteval CTStyle fg_from_hex(uint32_t hex) {
        if (hex > 0xFFFFFF) throw "fg<>: hex value exceeds 0xFFFFFF";
        return CTStyle{.has_fg=true,
            .fg_r=static_cast<uint8_t>(hex >> 16),
            .fg_g=static_cast<uint8_t>((hex >> 8) & 0xFF),
            .fg_b=static_cast<uint8_t>(hex & 0xFF)};
    }
    consteval CTStyle bg_from_hex(uint32_t hex) {
        if (hex > 0xFFFFFF) throw "bg<>: hex value exceeds 0xFFFFFF";
        return CTStyle{.has_bg=true,
            .bg_r=static_cast<uint8_t>(hex >> 16),
            .bg_g=static_cast<uint8_t>((hex >> 8) & 0xFF),
            .bg_b=static_cast<uint8_t>(hex & 0xFF)};
    }
}

template <uint32_t Hex>
inline constexpr StyTag<detail_color::fg_from_hex(Hex)> fg{};

template <uint32_t Hex>
inline constexpr StyTag<detail_color::bg_from_hex(Hex)> bg{};

// ── Style composition — StyTag | StyTag → merged StyTag ─────────────────────
//
// Pre-combine styles into reusable presets:
//   constexpr auto heading = Bold | fg<0xFFFFFF>;
//   constexpr auto error   = Bold | fg<0xFF4444>;
//   t<"Title"> | heading
//   text(msg) | error

template <CTStyle A, CTStyle B>
constexpr auto operator|(StyTag<A>, StyTag<B>) {
    return StyTag<A.merge(B)>{};
}

// ── Box layout config (structural NTTP) ──────────────────────────────────────

struct BoxCfg {
    int pad_t = 0, pad_r = 0, pad_b = 0, pad_l = 0;
    int gap = 0, grow = 0;
    BorderStyle bstyle = BorderStyle::None;
    bool has_border = false, has_border_color = false;
    uint8_t bc_r = 0, bc_g = 0, bc_b = 0;
    CTStyle style{};
    int ct_width = 0, ct_height = 0;  // compile-time fixed dimensions (0 = auto)
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
            if constexpr (Cfg.ct_width > 0)
                b = std::move(b).width(Dimension::fixed(Cfg.ct_width));
            if constexpr (Cfg.ct_height > 0)
                b = std::move(b).height(Dimension::fixed(Cfg.ct_height));
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

// ── operator| : RuntimeTextNode | trunc / nowrap ────────────────────────────

template <typename S>
[[nodiscard]] auto operator|(RuntimeTextNode<S> n, TruncateTag) {
    n.wrap = TextWrap::TruncateEnd;
    return n;
}

template <typename S>
[[nodiscard]] auto operator|(RuntimeTextNode<S> n, NoWrapTag) {
    n.wrap = TextWrap::NoWrap;
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
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b,
        Cfg.style.merge(V), Cfg.ct_width, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// ── operator| : Box | layout modifiers ──────────────────────────────────────

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int T, int R, int B, int L>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, PadTag<T, R, B, L>) {
    constexpr BoxCfg nc = {T, R, B, L, Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style,
        Cfg.ct_width, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int G>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, GapTag<G>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        G, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style,
        Cfg.ct_width, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, BorderStyle BS>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, BorderTag<BS>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, BS, true,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style,
        Cfg.ct_width, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// TYPE-STATE: border color requires border — applying bcol without border is
// a compile error. The requires clause enforces the state machine transition.
template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, uint8_t R, uint8_t G, uint8_t B>
    requires (Cfg.has_border)
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, BColTag<R, G, B>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        true, R, G, B, Cfg.style,
        Cfg.ct_width, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int G>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, GrowTag<G>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, G, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style,
        Cfg.ct_width, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// ── operator| : Box | width/height ──────────────────────────────────────────

template <int W>
struct WidthTag {
    static_assert(W > 0, "Width must be positive");
};

template <int H>
struct HeightTag {
    static_assert(H > 0, "Height must be positive");
};

template <int W> inline constexpr WidthTag<W>  w_{};
template <int H> inline constexpr HeightTag<H> h_{};

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int W>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, WidthTag<W>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style,
        W, Cfg.ct_height};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, int H>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, HeightTag<H>) {
    constexpr BoxCfg nc = {Cfg.pad_t, Cfg.pad_r, Cfg.pad_b, Cfg.pad_l,
        Cfg.gap, Cfg.grow, Cfg.bstyle, Cfg.has_border,
        Cfg.has_border_color, Cfg.bc_r, Cfg.bc_g, Cfg.bc_b, Cfg.style,
        Cfg.ct_width, H};
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// ── operator| : Text/Runtime | width (wraps in a fixed-width box) ──────────

template <Str S, CTStyle Sty, int W>
constexpr auto operator|(TextNode<S, Sty>, WidthTag<W>) {
    return BoxNode<FlexDirection::Row,
                   BoxCfg{.ct_width = W},
                   TextNode<S, Sty>>{{TextNode<S, Sty>{}}};
}

template <typename St, int W>
auto operator|(RuntimeTextNode<St> n, WidthTag<W>) {
    return BoxNode<FlexDirection::Row,
                   BoxCfg{.ct_width = W},
                   RuntimeTextNode<St>>{{std::move(n)}};
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

using maya::detail::box;
using maya::detail::vstack;
using maya::detail::hstack;
using maya::detail::center;
using maya::detail::zstack;
using maya::detail::component;

// ── User-defined literal for text nodes ─────────────────────────────────────
//
// C++26 string literal operator template — compile-time text via UDL:
//   "Hello"_t | Bold | fg<0x64B4FF>
//
// Equivalent to t<"Hello"> but reads more naturally in mixed expressions.

template <Str S>
constexpr auto operator""_t() { return TextNode<S>{}; }

// ── when() — conditional rendering ──────────────────────────────────────────
//
// Eliminates the dyn() boilerplate for simple show/hide:
//   when(is_loading, spinner, content)
//   when(show_debug, debug_panel)           // omit else → blank
//
// Both branches must satisfy Node. The condition is evaluated at build time.

template <Node Then, Node Else>
struct WhenNode {
    bool condition;
    Then then_branch;
    Else else_branch;

    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return condition ? then_branch.build() : else_branch.build();
    }
};

template <Node Then, Node Else>
[[nodiscard]] auto when(bool condition, Then&& then_node, Else&& else_node) {
    return WhenNode<std::decay_t<Then>, std::decay_t<Else>>{
        condition, std::forward<Then>(then_node), std::forward<Else>(else_node)};
}

template <Node Then>
[[nodiscard]] auto when(bool condition, Then&& then_node) {
    return WhenNode<std::decay_t<Then>, BlankNode>{
        condition, std::forward<Then>(then_node), BlankNode{}};
}

// ── visible() — runtime visibility pipe ─────────────────────────────────────
//
// Hide a node without removing it from the tree:
//   text("debug info") | visible(debug_mode)
//   panel | visible(expanded)

struct VisibleTag { bool show; };

[[nodiscard]] inline VisibleTag visible(bool show) { return {show}; }

template <Node N>
struct VisibleNode {
    N inner;
    bool show;

    operator Element() const { return build(); }
    [[nodiscard]] Element build() const {
        return show ? inner.build() : BlankNode{}.build();
    }
};

template <Node N>
[[nodiscard]] auto operator|(N n, VisibleTag tag) {
    return VisibleNode<N>{std::move(n), tag.show};
}

// ── bordered<Style, HexColor> — combined border + color pipe ────────────────
//
// Eliminates the two-step border_<Round> | bcol<R,G,B>:
//   v(...) | bordered<Round, 0x323746>
//   h(...) | bordered<Single>              // no color → default

namespace detail_border {
    consteval BoxCfg apply_bordered(BoxCfg cfg, BorderStyle bs,
                                    bool has_color, uint32_t hex) {
        cfg.bstyle = bs;
        cfg.has_border = true;
        if (has_color) {
            cfg.has_border_color = true;
            cfg.bc_r = static_cast<uint8_t>(hex >> 16);
            cfg.bc_g = static_cast<uint8_t>((hex >> 8) & 0xFF);
            cfg.bc_b = static_cast<uint8_t>(hex & 0xFF);
        }
        return cfg;
    }
}

template <BorderStyle BS, uint32_t Color = 0>
struct BorderedTag {
    static constexpr bool has_color = (Color != 0);
};

template <BorderStyle BS, uint32_t Color = 0>
inline constexpr BorderedTag<BS, Color> bordered{};

template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, BorderStyle BS, uint32_t Color>
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, BorderedTag<BS, Color>) {
    constexpr BoxCfg nc = detail_border::apply_bordered(
        Cfg, BS, BorderedTag<BS, Color>::has_color, Color);
    return BoxNode<Dir, nc, Cs...>{std::move(n.children)};
}

// ── Runtime pipe system ─────────────────────────────────────────────────────
//
// Runtime pipes for dynamic values — same | syntax as compile-time pipes:
//
//   Color c = theme.border;
//   auto ui = v(
//       t<"Status"> | Bold,
//       text(msg)
//   ) | border(Round) | bcolor(c) | btext("Info") | padding(0, 1);
//
// Compile-time:  pad<1>, border_<Round>, bcol<50,54,62>, grow_<1>
// Runtime:       padding(1), border(Round), bcolor(c), grow(1.0f)

// -- Runtime pipe tags --

struct RPad    { int t, r, b, l; };
struct RGap    { int g; };
struct RBorder { BorderStyle bs; };
struct RBCol   { Color c; };
struct RBText  { std::string s; BorderTextPos pos; BorderTextAlign align; };
struct RGrow   { float g; };
struct RWidth  { int w; };
struct RHeight { int h; };
struct RFg     { Color c; };
struct RBg     { Color c; };
struct RMargin { int t, r, b, l; };
struct RAlign  { Align a; };
struct RJust   { Justify j; };
struct ROvf    { Overflow o; };

// -- Runtime pipe factories --

[[nodiscard]] inline RPad    padding(int all)                  { return {all,all,all,all}; }
[[nodiscard]] inline RPad    padding(int v, int h)             { return {v,h,v,h}; }
[[nodiscard]] inline RPad    padding(int t,int r,int b,int l)  { return {t,r,b,l}; }
[[nodiscard]] inline RGap    gap(int g)                        { return {g}; }
[[nodiscard]] inline RBorder border(BorderStyle bs)            { return {bs}; }
[[nodiscard]] inline RBCol   bcolor(Color c)                   { return {c}; }
[[nodiscard]] inline RGrow   grow(float g = 1.0f)              { return {g}; }
[[nodiscard]] inline RWidth  width(int w)                      { return {w}; }
[[nodiscard]] inline RHeight height(int h)                     { return {h}; }
[[nodiscard]] inline RFg     fgc(Color c)                      { return {c}; }
[[nodiscard]] inline RBg     bgc(Color c)                      { return {c}; }
[[nodiscard]] inline RMargin margin(int all)                    { return {all,all,all,all}; }
[[nodiscard]] inline RMargin margin(int v, int h)               { return {v,h,v,h}; }
[[nodiscard]] inline RMargin margin(int t,int r,int b,int l)    { return {t,r,b,l}; }
[[nodiscard]] inline RAlign  align(Align a)                     { return {a}; }
[[nodiscard]] inline RJust   justify(Justify j)                 { return {j}; }
[[nodiscard]] inline ROvf    overflow(Overflow o)               { return {o}; }

[[nodiscard]] inline RBText btext(std::string s,
    BorderTextPos p = BorderTextPos::Top,
    BorderTextAlign a = BorderTextAlign::Start) {
    return {std::move(s), p, a};
}

// -- WrappedNode: runtime box properties layered on any Node --

template <Node Inner>
struct WrappedNode {
    static constexpr bool is_wrapped_tag_ = true;
    static constexpr uint16_t PAD=1,GAP=2,BRD=4,BCOL=8,BTXT=16,
                              GRW=32,WD=64,HT=128,STY=256,
                              MGN=512,ALN=1024,JST=2048,OVF=4096;

    Inner inner;
    int pt_=0, pr_=0, pb_=0, pl_=0, gap_=0;
    BorderStyle brd_{};
    Color bcol_{};
    std::string btxt_ = {};
    BorderTextPos btp_{};
    BorderTextAlign bta_{};
    float grw_=0;
    int w_=0, h_=0;
    Style sty_{};
    int mt_=0, mr_=0, mb_=0, ml_=0;
    Align aln_{};
    Justify jst_{};
    Overflow ovf_{};
    uint16_t f_=0;

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Build the inner first. If it's already a BoxElement, apply our
        // runtime modifiers in-place — adding a wrapper layer breaks
        // align_items propagation (Stretch only stretches the wrapper's
        // single child to wrapper width, not the grandchildren to their
        // intended width). For non-box inners (text, etc.) we still need
        // an outer box to host the runtime properties.
        Element inner_elem = inner.build();
        if (auto* box = maya::as_box(inner_elem)) {
            if (f_&PAD)  box->layout.padding = Edges<int>(pt_, pr_, pb_, pl_);
            if (f_&GAP)  box->layout.gap = gap_;
            if (f_&BRD)  box->border.style = brd_;
            if (f_&BCOL) box->border.colors = BorderColors::uniform(bcol_);
            if (f_&BTXT) box->border.text = BorderText{btxt_, btp_, bta_, 0};
            if (f_&GRW)  box->layout.grow = grw_;
            if (f_&WD)   box->layout.width = Dimension::fixed(w_);
            if (f_&HT)   box->layout.height = Dimension::fixed(h_);
            if (f_&STY)  box->style = box->style.merge(sty_);
            if (f_&MGN)  box->layout.margin = Edges<int>(mt_, mr_, mb_, ml_);
            if (f_&ALN)  box->layout.align_items = aln_;
            if (f_&JST)  box->layout.justify = jst_;
            if (f_&OVF)  box->overflow = ovf_;
            return inner_elem;
        }
        auto b = maya::detail::box();
        if (f_&PAD)  b.padding(pt_,pr_,pb_,pl_);
        if (f_&GAP)  b.gap(gap_);
        if (f_&BRD)  b.border(brd_);
        if (f_&BCOL) b.border_color(bcol_);
        if (f_&BTXT) b.border_text(btxt_,btp_,bta_);
        if (f_&GRW)  b.grow(grw_);
        if (f_&WD)   b.width(Dimension::fixed(w_));
        if (f_&HT)   b.height(Dimension::fixed(h_));
        if (f_&STY)  b.style(sty_);
        if (f_&MGN)  b.margin(mt_,mr_,mb_,ml_);
        if (f_&ALN)  b.align_items(aln_);
        if (f_&JST)  b.justify(jst_);
        if (f_&OVF)  b.overflow(ovf_);
        return b(std::move(inner_elem));
    }
};

template <Node N>
auto as_wrapped(N n) {
    if constexpr (requires { N::is_wrapped_tag_; }) {
        return std::move(n);
    } else {
        return WrappedNode<N>{std::move(n)};
    }
}

// -- operator| : Node | runtime pipe → WrappedNode --

template <Node N> [[nodiscard]] auto operator|(N n, RPad t) {
    auto w = as_wrapped(std::move(n));
    w.pt_=t.t; w.pr_=t.r; w.pb_=t.b; w.pl_=t.l;
    w.f_ |= decltype(w)::PAD; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RGap t) {
    auto w = as_wrapped(std::move(n));
    w.gap_=t.g; w.f_ |= decltype(w)::GAP; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RBorder t) {
    auto w = as_wrapped(std::move(n));
    w.brd_=t.bs; w.f_ |= decltype(w)::BRD; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RBCol t) {
    auto w = as_wrapped(std::move(n));
    w.bcol_=t.c; w.f_ |= decltype(w)::BCOL; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RBText t) {
    auto w = as_wrapped(std::move(n));
    w.btxt_=std::move(t.s); w.btp_=t.pos; w.bta_=t.align;
    w.f_ |= decltype(w)::BTXT; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RGrow t) {
    auto w = as_wrapped(std::move(n));
    w.grw_=t.g; w.f_ |= decltype(w)::GRW; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RWidth t) {
    auto w = as_wrapped(std::move(n));
    w.w_=t.w; w.f_ |= decltype(w)::WD; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RHeight t) {
    auto w = as_wrapped(std::move(n));
    w.h_=t.h; w.f_ |= decltype(w)::HT; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RFg t) {
    auto w = as_wrapped(std::move(n));
    w.sty_ = w.sty_.with_fg(t.c); w.f_ |= decltype(w)::STY; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RBg t) {
    auto w = as_wrapped(std::move(n));
    w.sty_ = w.sty_.with_bg(t.c); w.f_ |= decltype(w)::STY; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RMargin t) {
    auto w = as_wrapped(std::move(n));
    w.mt_=t.t; w.mr_=t.r; w.mb_=t.b; w.ml_=t.l;
    w.f_ |= decltype(w)::MGN; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RAlign t) {
    auto w = as_wrapped(std::move(n));
    w.aln_=t.a; w.f_ |= decltype(w)::ALN; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, RJust t) {
    auto w = as_wrapped(std::move(n));
    w.jst_=t.j; w.f_ |= decltype(w)::JST; return w;
}
template <Node N> [[nodiscard]] auto operator|(N n, ROvf t) {
    auto w = as_wrapped(std::move(n));
    w.ovf_=t.o; w.f_ |= decltype(w)::OVF; return w;
}

// -- Compile-time pipe tags forwarded through WrappedNode --

template <Node N, CTStyle V>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, StyTag<V>) {
    n.sty_ = n.sty_.merge(V.runtime());
    n.f_ |= decltype(n)::STY; return n;
}

template <Node N, int T, int R, int B, int L>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, PadTag<T,R,B,L>) {
    n.pt_=T; n.pr_=R; n.pb_=B; n.pl_=L;
    n.f_ |= decltype(n)::PAD; return n;
}

template <Node N, int G>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, GapTag<G>) {
    n.gap_=G; n.f_ |= decltype(n)::GAP; return n;
}

template <Node N, BorderStyle BS>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, BorderTag<BS>) {
    n.brd_=BS; n.f_ |= decltype(n)::BRD; return n;
}

template <Node N, uint8_t R, uint8_t G, uint8_t B>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, BColTag<R,G,B>) {
    n.bcol_=Color::rgb(R,G,B); n.f_ |= decltype(n)::BCOL; return n;
}

template <Node N, int G>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, GrowTag<G>) {
    n.grw_=static_cast<float>(G); n.f_ |= decltype(n)::GRW; return n;
}

template <Node N, int W>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, WidthTag<W>) {
    n.w_=W; n.f_ |= decltype(n)::WD; return n;
}

template <Node N, int H>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, HeightTag<H>) {
    n.h_=H; n.f_ |= decltype(n)::HT; return n;
}

template <Node N, BorderStyle BS, uint32_t C>
    requires (requires { N::is_wrapped_tag_; })
[[nodiscard]] auto operator|(N n, BorderedTag<BS, C>) {
    n.brd_=BS; n.f_ |= decltype(n)::BRD;
    if constexpr (C != 0) {
        n.bcol_=Color::rgb(static_cast<uint8_t>(C>>16),
                           static_cast<uint8_t>((C>>8)&0xFF),
                           static_cast<uint8_t>(C&0xFF));
        n.f_ |= decltype(n)::BCOL;
    }
    return n;
}

// ── each() — apply a style to every item in a range ─────────────────────────
//
// Shorthand for map() when you just want to style range items:
//   each(items, [](auto& s) { return text(s); }) | Bold
//
// (This is just an alias for map() — included for discoverability.)

template <std::ranges::range R, typename Proj>
[[nodiscard]] auto each(R&& range, Proj&& proj) {
    return map(std::forward<R>(range), std::forward<Proj>(proj));
}

// ── Compile-time validation ─────────────────────────────────────────────────

static_assert(Node<TextNode<Str{"hello"}>>,              "TextNode must satisfy Node");
static_assert(Node<RuntimeTextNode<std::string_view>>,   "RuntimeTextNode must satisfy Node");
static_assert(Node<RuntimeTextNode<int>>,                "RuntimeTextNode<int> must satisfy Node");
static_assert(Node<SpacerNode>,                          "SpacerNode must satisfy Node");
static_assert(Node<SepNode>,                             "SepNode must satisfy Node");
static_assert(Node<VSepNode>,                            "VSepNode must satisfy Node");
static_assert(Node<BlankNode>,                           "BlankNode must satisfy Node");
static_assert(Node<WrappedNode<BlankNode>>,              "WrappedNode must satisfy Node");

} // namespace maya::dsl
