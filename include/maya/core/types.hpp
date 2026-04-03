#pragma once
// maya::core::types - Strong types, phantom types, type-state tokens
//
// The foundation of maya's compile-time safety. Every distinct semantic
// quantity gets its own type. You cannot accidentally pass Columns where
// Rows are expected, or write to a terminal that hasn't entered raw mode.

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace maya {

// ============================================================================
// Strong<Tag, T> - Zero-cost newtype wrapper
// ============================================================================
// Rust has newtype structs. We have this. The Tag type is a phantom -
// it exists only at compile time to prevent mixing distinct quantities.
//
// Strong<ColumnTag, int> and Strong<RowTag, int> are completely unrelated
// types. No implicit conversion. No accidental mixing. Period.

template <typename Tag, typename T = int>
struct Strong {
    T value;

    constexpr explicit Strong(T v) noexcept : value(v) {}
    constexpr Strong() noexcept : value{} {}

    constexpr auto operator<=>(const Strong&) const = default;

    // Arithmetic preserves the tag - Columns + Columns = Columns
    constexpr Strong operator+(Strong rhs) const noexcept { return Strong{value + rhs.value}; }
    constexpr Strong operator-(Strong rhs) const noexcept { return Strong{value - rhs.value}; }
    constexpr Strong operator*(T scalar) const noexcept { return Strong{value * scalar}; }
    constexpr Strong operator/(T scalar) const noexcept { return Strong{value / scalar}; }
    constexpr Strong operator%(T scalar) const noexcept { return Strong{value % scalar}; }

    constexpr Strong& operator+=(Strong rhs) noexcept { value += rhs.value; return *this; }
    constexpr Strong& operator-=(Strong rhs) noexcept { value -= rhs.value; return *this; }

    // Explicit unwrap - make the conversion intentional
    constexpr explicit operator T() const noexcept { return value; }

    // Named accessor for clarity
    [[nodiscard]] constexpr T raw() const noexcept { return value; }
};

// Scalar * Strong works too
template <typename Tag, typename T>
constexpr Strong<Tag, T> operator*(T scalar, Strong<Tag, T> s) noexcept {
    return Strong<Tag, T>{scalar * s.value};
}

// ============================================================================
// Dimension Tags
// ============================================================================

struct ColumnTag {};
struct RowTag {};

using Columns = Strong<ColumnTag>;
using Rows    = Strong<RowTag>;

// ============================================================================
// Size - a (width, height) pair in terminal cells
// ============================================================================

struct Size {
    Columns width;
    Rows    height;

    constexpr auto operator<=>(const Size&) const = default;

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return width.value == 0 && height.value == 0;
    }

    [[nodiscard]] constexpr int area() const noexcept {
        return width.value * height.value;
    }
};

// ============================================================================
// Position - (x, y) in terminal cells
// ============================================================================

struct Position {
    Columns x;
    Rows    y;

    constexpr auto operator<=>(const Position&) const = default;

    [[nodiscard]] static constexpr Position origin() noexcept {
        return {Columns{0}, Rows{0}};
    }
};

// ============================================================================
// Rect - position + size
// ============================================================================

struct Rect {
    Position pos;
    Size     size;

    [[nodiscard]] constexpr Columns left()   const noexcept { return pos.x; }
    [[nodiscard]] constexpr Rows    top()    const noexcept { return pos.y; }
    [[nodiscard]] constexpr Columns right()  const noexcept { return pos.x + size.width; }
    [[nodiscard]] constexpr Rows    bottom() const noexcept { return pos.y + size.height; }

    [[nodiscard]] constexpr bool contains(Position p) const noexcept {
        return p.x >= left() && p.x < right()
            && p.y >= top()  && p.y < bottom();
    }

    [[nodiscard]] constexpr Rect intersect(const Rect& other) const noexcept {
        int l = std::max(left().value, other.left().value);
        int t = std::max(top().value, other.top().value);
        int r = std::min(right().value, other.right().value);
        int b = std::min(bottom().value, other.bottom().value);
        if (l >= r || t >= b) return {Position::origin(), Size{Columns(0), Rows(0)}};
        return {Position{Columns(l), Rows(t)}, Size{Columns(r - l), Rows(b - t)}};
    }

    [[nodiscard]] constexpr Rect unite(const Rect& other) const noexcept {
        if (size.is_zero()) return other;
        if (other.size.is_zero()) return *this;
        int l = std::min(left().value, other.left().value);
        int t = std::min(top().value, other.top().value);
        int r = std::max(right().value, other.right().value);
        int b = std::max(bottom().value, other.bottom().value);
        return {Position{Columns(l), Rows(t)}, Size{Columns(r - l), Rows(b - t)}};
    }

    constexpr auto operator<=>(const Rect&) const = default;
};

// ============================================================================
// Type-state tokens for terminal mode transitions
// ============================================================================
// These are zero-size types used as template parameters to encode
// the terminal's state in the type system. A Terminal<Raw> cannot
// be constructed except by moving a Terminal<Cooked> through
// enable_raw_mode(). This is the type-state pattern.

struct Cooked    {};   // Default terminal mode
struct Raw       {};   // Raw mode enabled (no line buffering, no echo)
struct AltScreen {};   // Alternate screen buffer active

// ============================================================================
// Edges - for margin, padding, border per-side values
// ============================================================================

enum class Edge : uint8_t {
    Top    = 0,
    Right  = 1,
    Bottom = 2,
    Left   = 3,
};

template <typename T>
struct Edges {
    T top{};
    T right{};
    T bottom{};
    T left{};

    constexpr Edges() = default;

    // Uniform
    constexpr explicit Edges(T all)
        : top(all), right(all), bottom(all), left(all) {}

    // Vertical, Horizontal
    constexpr Edges(T vertical, T horizontal)
        : top(vertical), right(horizontal), bottom(vertical), left(horizontal) {}

    // Top, Horizontal, Bottom
    constexpr Edges(T t, T horizontal, T b)
        : top(t), right(horizontal), bottom(b), left(horizontal) {}

    // All four
    constexpr Edges(T t, T r, T b, T l)
        : top(t), right(r), bottom(b), left(l) {}

    [[nodiscard]] constexpr T& operator[](Edge e) noexcept {
        switch (e) {
            case Edge::Top:    return top;
            case Edge::Right:  return right;
            case Edge::Bottom: return bottom;
            case Edge::Left:   return left;
        }
        std::unreachable();
    }

    [[nodiscard]] constexpr const T& operator[](Edge e) const noexcept {
        switch (e) {
            case Edge::Top:    return top;
            case Edge::Right:  return right;
            case Edge::Bottom: return bottom;
            case Edge::Left:   return left;
        }
        std::unreachable();
    }

    [[nodiscard]] constexpr T horizontal() const noexcept { return left + right; }
    [[nodiscard]] constexpr T vertical()   const noexcept { return top + bottom; }

    constexpr auto operator<=>(const Edges&) const = default;
};

// ============================================================================
// Dimension - Auto | Fixed(int) | Percent(float)
// ============================================================================
// Used for width, height, flex-basis. A sum type (algebraic data type).

struct Auto {};

struct Dimension {
    enum class Kind : uint8_t { Auto, Fixed, Percent };

    Kind  kind;
    float value;

    constexpr Dimension() noexcept : kind(Kind::Auto), value(0) {}
    constexpr Dimension(Auto) noexcept : kind(Kind::Auto), value(0) {}
    constexpr Dimension(int v) noexcept : kind(Kind::Fixed), value(static_cast<float>(v)) {}
    constexpr Dimension(Columns c) noexcept : kind(Kind::Fixed), value(static_cast<float>(c.value)) {}

    [[nodiscard]] static constexpr Dimension auto_() noexcept { return {}; }
    [[nodiscard]] static constexpr Dimension fixed(int v) noexcept { return Dimension{v}; }
    [[nodiscard]] static constexpr Dimension percent(float p) noexcept {
        Dimension d;
        d.kind = Kind::Percent;
        d.value = p;
        return d;
    }

    [[nodiscard]] constexpr bool is_auto()    const noexcept { return kind == Kind::Auto; }
    [[nodiscard]] constexpr bool is_fixed()   const noexcept { return kind == Kind::Fixed; }
    [[nodiscard]] constexpr bool is_percent() const noexcept { return kind == Kind::Percent; }

    // Resolve against a parent dimension
    [[nodiscard]] constexpr int resolve(int parent) const noexcept {
        switch (kind) {
            case Kind::Auto:    return parent;
            case Kind::Fixed:   return static_cast<int>(value);
            case Kind::Percent: return static_cast<int>(value * static_cast<float>(parent) / 100.0f);
        }
        std::unreachable();
    }
};

// User-defined literal: 50_pct
consteval Dimension operator""_pct(unsigned long long v) {
    return Dimension::percent(static_cast<float>(v));
}

// ============================================================================
// Compile-time string (for consteval ANSI generation)
// ============================================================================

template <std::size_t N>
struct FixedString {
    char data[N]{};
    std::size_t len = 0;

    constexpr FixedString() = default;

    consteval FixedString(const char (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = str[i];
        len = N - 1; // exclude null
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {data, len};
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return view();
    }
};

// ============================================================================
// Utility: move-only type enforcer
// ============================================================================

struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};

} // namespace maya
