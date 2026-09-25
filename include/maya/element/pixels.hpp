#pragma once
// maya/element/pixels.hpp — an image as an element.
//
// A terminal cell is two pixels tall when drawn with the upper half block
// '▀' (foreground = top pixel, background = bottom pixel). Every animation
// maya shipped (fires, fluids, fractals, ray tracers) did that packing by
// hand, AND pre-interned a style for every colour pair it might use, AND
// kept the style ids in globals, AND re-interned them on resize: a canvas
// loop's worth of bookkeeping standing between "I have a picture" and "it's
// on the screen".
//
// pixels() is the picture:
//
//     static Element view(const Model& m) {
//         return pixels(m.img);                    // an Image: w x h RGB
//     }
//
// It fills the space layout gives it. The image is sampled to that size
// (nearest neighbour), so a model can keep a fixed-resolution buffer or
// size itself to the screen (see Image::fit): either way the view is a pure
// function of the model.
//
// Cost: one style lookup per cell through a small direct-mapped cache keyed
// on the packed (top, bottom) colour pair, owned by the renderer's pool
// (so a pool rebase invalidates it), and the frame diff sends only cells
// that changed. No interning by the user, no ids in the model.

#include <cstdint>
#include <memory>
#include <utility>
#include <thread>
#include <vector>

#include "builder.hpp"
#include "../render/canvas.hpp"

namespace maya {

/// 24-bit colour, the unit of an Image. Plain value; 0x00RRGGBB order.
struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    constexpr bool operator==(const Rgb&) const = default;
    [[nodiscard]] constexpr std::uint32_t packed() const noexcept {
        return (std::uint32_t{r} << 16) | (std::uint32_t{g} << 8) | b;
    }
};

/// A width x height RGB buffer in row-major order. A value type: copy it,
/// keep it in a Model, compare it.
class Image {
public:
    Image() = default;
    Image(int w, int h, Rgb fill = {}) : w_(std::max(0, w)), h_(std::max(0, h)),
        px_(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), fill) {}

    [[nodiscard]] int width()  const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }
    [[nodiscard]] bool empty() const noexcept { return px_.empty(); }

    [[nodiscard]] Rgb&       operator()(int x, int y)       noexcept { return px_[idx(x, y)]; }
    [[nodiscard]] const Rgb& operator()(int x, int y) const noexcept { return px_[idx(x, y)]; }
    [[nodiscard]] std::vector<Rgb>&       data()       noexcept { return px_; }
    [[nodiscard]] const std::vector<Rgb>& data() const noexcept { return px_; }

    void fill(Rgb c) { std::fill(px_.begin(), px_.end(), c); }

    /// Compute every pixel with `fn(x, y) -> Rgb`, rows spread over the
    /// machine's cores. For views whose picture is a pure per-pixel
    /// function of the model (a fractal, a ray tracer, a shader): the
    /// function only reads, so running rows in parallel is safe, and
    /// rows are INTERLEAVED across threads (thread t takes t, t+n, ...),
    /// because cost varies by region (sky is cheap, terrain is not) and
    /// contiguous bands leave most threads idle behind the slowest.
    template <class Fn>
    void fill_rows(Fn&& fn) {
        const int n = std::max(1, std::min(static_cast<int>(std::thread::hardware_concurrency()), h_));
        auto rows = [&](int first) {
            for (int y = first; y < h_; y += n)
                for (int x = 0; x < w_; ++x) px_[idx(x, y)] = fn(x, y);
        };
        if (n == 1 || static_cast<long>(w_) * h_ < 4096) { for (int y = 0; y < h_; ++y) for (int x = 0; x < w_; ++x) px_[idx(x, y)] = fn(x, y); return; }
        std::vector<std::jthread> pool;
        pool.reserve(static_cast<std::size_t>(n - 1));
        for (int t = 1; t < n; ++t) pool.emplace_back(rows, t);
        rows(0);
    }

    /// The pixel size of a cell area: w columns, 2*h rows (half blocks).
    [[nodiscard]] static constexpr std::pair<int, int> for_cells(int cols, int rows) noexcept {
        return {cols, rows * 2};
    }

    bool operator==(const Image&) const = default;

private:
    [[nodiscard]] std::size_t idx(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
    }
    int w_ = 0, h_ = 0;
    std::vector<Rgb> px_;
};

namespace detail {

// Colour pair -> style id, memoised per pool generation. Direct-mapped: a
// miss interns (the pool dedups), a hit is two compares. Sized for an
// animation's working set (a fire uses a few hundred pairs; a gradient
// image a few thousand).
class PixelStyles {
public:
    std::uint16_t get(StylePool& pool, Rgb top, Rgb bot, bool bold = false) {
        if (pool.pool_id() != pool_id_) { slots_.assign(kSlots, Slot{}); pool_id_ = pool.pool_id(); }
        const std::uint64_t key = (std::uint64_t{bold} << 48) | (std::uint64_t{top.packed()} << 24) | bot.packed();
        const std::size_t   i   = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> (64 - kBits));
        Slot& s = slots_[i];
        if (s.live && s.key == key) return s.id;
        Style st = Style{}.with_fg(Color::rgb(top.r, top.g, top.b)).with_bg(Color::rgb(bot.r, bot.g, bot.b));
        if (bold) st = st.with_bold();
        s = {key, pool.intern(st), true};
        return s.id;
    }
private:
    static constexpr int         kBits  = 14;
    static constexpr std::size_t kSlots = std::size_t{1} << kBits;
    struct Slot { std::uint64_t key = 0; std::uint16_t id = 0; bool live = false; };
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);
    std::uint64_t     pool_id_ = 0;
};
inline PixelStyles& pixel_styles() { thread_local PixelStyles p; return p; }

// Paint `img` into the w x h cell rectangle at (x0, y0), sampling it to
// w x 2h pixels.
inline void paint_pixels(Canvas& c, const Image& img, int x0, int y0, int w, int h) {
    if (img.empty() || w <= 0 || h <= 0) return;
    StylePool& pool = *c.style_pool();
    auto& styles = pixel_styles();
    const int iw = img.width(), ih = img.height(), ph = h * 2;
    const bool exact = (iw == w && ih == ph);
    for (int cy = 0; cy < h; ++cy) {
        const int ty = exact ? cy * 2     : (cy * 2) * ih / ph;
        const int by = exact ? cy * 2 + 1 : (cy * 2 + 1) * ih / ph;
        for (int cx = 0; cx < w; ++cx) {
            const int sx = exact ? cx : cx * iw / w;
            c.set(x0 + cx, y0 + cy, U'\u2580', styles.get(pool, img(sx, ty), img(sx, by)));
        }
    }
}

}  // namespace detail

/// An image that fills its slot, two pixels per cell.
///
/// Holds the image by shared ownership: building the view copies a pointer,
/// not the pixels, and the element stays valid for as long as the renderer
/// needs it (the model can move on).
[[nodiscard]] inline auto pixels(std::shared_ptr<const Image> img) -> ComponentBuilder {
    return detail::paint([img = std::move(img)](Canvas& c, int x, int y, int w, int h) {
        detail::paint_pixels(c, *img, x, y, w, h);
    });
}

/// Convenience: copy an image into the element.
[[nodiscard]] inline auto pixels(Image img) -> ComponentBuilder {
    return pixels(std::make_shared<const Image>(std::move(img)));
}

// ── Glyphs: a grid of coloured characters ───────────────────────────────────────────
//
// The text twin of Image. Character-art programs (digital rain, a starfield
// of '.', '*' and '+', a spectrum of block glyphs) are a cols x rows grid of
// (character, colour) cells, and each used to paint it with pre-interned
// style ids. glyphs() is that grid as an element: set cells in the view,
// return glyphs(std::move(grid)). Cells with ch == 0 are left untouched, so
// a sparse grid can sit over something else in a zstack.

struct Glyph {
    char32_t ch   = U' ';
    Rgb      fg   {200, 200, 200};
    Rgb      bg   {0, 0, 0};
    bool     bold = false;
    constexpr bool operator==(const Glyph&) const = default;
};

class Glyphs {
public:
    Glyphs() = default;
    Glyphs(int cols, int rows, Glyph fill = {}) : w_(std::max(0, cols)), h_(std::max(0, rows)),
        g_(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), fill) {}
    [[nodiscard]] int width()  const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }
    [[nodiscard]] Glyph&       operator()(int x, int y)       noexcept { return g_[idx(x, y)]; }
    [[nodiscard]] const Glyph& operator()(int x, int y) const noexcept { return g_[idx(x, y)]; }
    [[nodiscard]] bool in(int x, int y) const noexcept { return x >= 0 && y >= 0 && x < w_ && y < h_; }
    /// Write a string starting at (x, y), clipped to the grid.
    void text(int x, int y, std::u32string_view s, Rgb fg, Rgb bg = {0, 0, 0}, bool bold = false) {
        for (std::size_t i = 0; i < s.size(); ++i)
            if (in(x + static_cast<int>(i), y)) (*this)(x + static_cast<int>(i), y) = {s[i], fg, bg, bold};
    }
    bool operator==(const Glyphs&) const = default;
private:
    [[nodiscard]] std::size_t idx(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
    }
    int w_ = 0, h_ = 0;
    std::vector<Glyph> g_;
};

namespace detail {
inline void paint_glyphs(Canvas& c, const Glyphs& g, int x0, int y0, int w, int h) {
    StylePool& pool = *c.style_pool();
    auto& styles = pixel_styles();
    const int cw = std::min(w, g.width()), ch = std::min(h, g.height());
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const Glyph& gl = g(x, y);
            if (gl.ch == 0) continue;
            c.set(x0 + x, y0 + y, gl.ch, styles.get(pool, gl.fg, gl.bg, gl.bold));
        }
}
}  // namespace detail

/// A character grid that fills its slot. The grid is drawn from the top
/// left; size it to the slot in update() (a Resize message) like an Image.
[[nodiscard]] inline auto glyphs(Glyphs grid) -> ComponentBuilder {
    return detail::paint([g = std::make_shared<const Glyphs>(std::move(grid))]
                         (Canvas& c, int x, int y, int w, int h) { detail::paint_glyphs(c, *g, x, y, w, h); });
}

}  // namespace maya
