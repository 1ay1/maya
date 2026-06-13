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

class Color {
public:
    enum class Kind : uint8_t { Named, Indexed, Rgb, Default };

private:
    Kind    kind_;
    uint8_t r_, g_, b_;  // For Rgb; r_ doubles as index for Named/Indexed

public:
    // Constructors - all constexpr
    constexpr Color() noexcept : kind_(Kind::Named), r_(7), g_(0), b_(0) {} // default white

    constexpr explicit Color(AnsiColor c) noexcept
        : kind_(Kind::Named), r_(static_cast<uint8_t>(c)), g_(0), b_(0) {}

    // Named color factories
    static constexpr Color black()          noexcept { return Color{AnsiColor::Black}; }
    static constexpr Color red()            noexcept { return Color{AnsiColor::Red}; }
    static constexpr Color green()          noexcept { return Color{AnsiColor::Green}; }
    static constexpr Color yellow()         noexcept { return Color{AnsiColor::Yellow}; }
    static constexpr Color blue()           noexcept { return Color{AnsiColor::Blue}; }
    static constexpr Color magenta()        noexcept { return Color{AnsiColor::Magenta}; }
    static constexpr Color cyan()           noexcept { return Color{AnsiColor::Cyan}; }
    static constexpr Color white()          noexcept { return Color{AnsiColor::White}; }
    static constexpr Color bright_black()   noexcept { return Color{AnsiColor::BrightBlack}; }
    static constexpr Color bright_red()     noexcept { return Color{AnsiColor::BrightRed}; }
    static constexpr Color bright_green()   noexcept { return Color{AnsiColor::BrightGreen}; }
    static constexpr Color bright_yellow()  noexcept { return Color{AnsiColor::BrightYellow}; }
    static constexpr Color bright_blue()    noexcept { return Color{AnsiColor::BrightBlue}; }
    static constexpr Color bright_magenta() noexcept { return Color{AnsiColor::BrightMagenta}; }
    static constexpr Color bright_cyan()    noexcept { return Color{AnsiColor::BrightCyan}; }
    static constexpr Color bright_white()   noexcept { return Color{AnsiColor::BrightWhite}; }
    static constexpr Color gray()           noexcept { return bright_black(); }
    static constexpr Color grey()           noexcept { return bright_black(); }

    /// Terminal-default color (SGR 39 fg / 49 bg). Use this when you want a
    /// container to occlude underlying cells in a zstack while still showing
    /// the user's terminal theme background.
    static constexpr Color default_color() noexcept {
        Color c;
        c.kind_ = Kind::Default;
        c.r_ = 0;
        return c;
    }

    // 256-color palette
    static constexpr Color indexed(uint8_t index) noexcept {
        Color c;
        c.kind_ = Kind::Indexed;
        c.r_ = index;
        return c;
    }

    // 24-bit truecolor
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        Color c;
        c.kind_ = Kind::Rgb;
        c.r_ = r;
        c.g_ = g;
        c.b_ = b;
        return c;
    }

    // From hex literal: Color::hex(0xFF00FF)
    static consteval Color hex(uint32_t rgb) {
        if (rgb > 0xFFFFFF) throw "Color::hex: value exceeds 0xFFFFFF";
        return Color::rgb(
            static_cast<uint8_t>((rgb >> 16) & 0xFF),
            static_cast<uint8_t>((rgb >> 8) & 0xFF),
            static_cast<uint8_t>(rgb & 0xFF)
        );
    }

    // HSL to RGB conversion (constexpr)
    static constexpr Color hsl(float h, float s, float l) noexcept {
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
        return Color::rgb(
            static_cast<uint8_t>(r * 255 + 0.5f),
            static_cast<uint8_t>(g * 255 + 0.5f),
            static_cast<uint8_t>(b * 255 + 0.5f)
        );
    }

    // Accessors
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr uint8_t r() const noexcept { return r_; }
    [[nodiscard]] constexpr uint8_t g() const noexcept { return g_; }
    [[nodiscard]] constexpr uint8_t b() const noexcept { return b_; }
    [[nodiscard]] constexpr uint8_t index() const noexcept { return r_; }

    // Downgrade this color to what a terminal of the given capability `level`
    // can actually display:  3 = truecolor (unchanged), 2 = 256-color,
    // 1 = 16-color.  RGB and 256-indexed colors are mapped to the nearest
    // representable color so apps look right on terminals without 24-bit
    // support — most importantly macOS Terminal.app, which is 256-color only
    // and silently drops `38;2` truecolor escapes. Named/Default colors and
    // anything already within the terminal's range pass through untouched.
    // Color is never stripped here; honoring "no color at all" is the
    // caller's job (it simply omits the SGR).
    [[nodiscard]] constexpr Color degrade(int level) const noexcept {
        if (level >= 3) return *this;
        switch (kind_) {
            case Kind::Default:
            case Kind::Named:
                return *this;
            case Kind::Indexed: {
                if (level >= 2) return *this;
                detail::Rgb3 c = detail::xterm256_to_rgb(r_);
                return Color{static_cast<AnsiColor>(
                    detail::rgb_to_ansi16(c.r, c.g, c.b))};
            }
            case Kind::Rgb:
                if (level >= 2)
                    return Color::indexed(detail::rgb_to_xterm256(r_, g_, b_));
                return Color{static_cast<AnsiColor>(
                    detail::rgb_to_ansi16(r_, g_, b_))};
        }
        return *this;
    }

    // Generate foreground SGR codes
    [[nodiscard]] std::string fg_sgr() const {
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
            case Kind::Default:
                return "39";
        }
        __builtin_unreachable();
    }

    // Generate background SGR codes
    [[nodiscard]] std::string bg_sgr() const {
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
            case Kind::Default:
                return "49";
        }
        __builtin_unreachable();
    }

    // -------------------------------------------------------------------------
    // Zero-allocation SGR emitters — write directly into an existing string.
    // Avoids any heap allocation on the hot rendering path.
    // -------------------------------------------------------------------------

    void append_fg_sgr(std::string& out) const {
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
            case Kind::Default:
                out += "39";
                break;
        }
    }

    void append_bg_sgr(std::string& out) const {
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
            case Kind::Default:
                out += "49";
                break;
        }
    }

    // Lighten/darken (returns new color)
    [[nodiscard]] constexpr Color lighten(float amount) const noexcept {
        if (kind_ != Kind::Rgb) return *this;
        auto lift = [amount](uint8_t c) -> uint8_t {
            float v = static_cast<float>(c) / 255.0f;
            v += (1.0f - v) * amount;
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255);
        };
        return Color::rgb(lift(r_), lift(g_), lift(b_));
    }

    [[nodiscard]] constexpr Color darken(float amount) const noexcept {
        if (kind_ != Kind::Rgb) return *this;
        auto drop = [amount](uint8_t c) -> uint8_t {
            float v = static_cast<float>(c) / 255.0f;
            v *= (1.0f - amount);
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255);
        };
        return Color::rgb(drop(r_), drop(g_), drop(b_));
    }

    constexpr auto operator<=>(const Color&) const = default;
};

// Compile-time validation
static_assert(Color::hex(0xFF00FF).r() == 255);
static_assert(Color::hex(0xFF00FF).g() == 0);
static_assert(Color::hex(0xFF00FF).b() == 255);
static_assert(Color::red().kind() == Color::Kind::Named);
static_assert(Color::indexed(42).index() == 42);

} // namespace maya
