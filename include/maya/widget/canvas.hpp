#pragma once
// maya::widget::canvas — Freeform drawing surface using half-block characters
//
// Each terminal cell holds two vertical pixels rendered via half-block
// characters: upper half (U+2580), lower half (U+2584), full block (U+2588),
// or a space. The foreground color encodes the top pixel and the background
// color encodes the bottom pixel, giving double vertical resolution.
//
// Resolution: width_ x (height_ * 2) pixels.
//
// Usage:
//   PixelCanvas c(40, 10);
//   c.set_pixel(5, 3, Color::red());
//   c.line(0, 0, 39, 19, Color::blue());
//   auto ui = c.build();

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

class PixelCanvas {
    int width_  = 0;  // cell width
    int height_ = 0;  // cell height (pixel height = height_ * 2)
    std::vector<Color> top_pixels_;     // top half of each cell
    std::vector<Color> bottom_pixels_;  // bottom half of each cell
    Color bg_ = Color::black();   // default background / clear color

    [[nodiscard]] size_t idx(int x, int y_cell) const {
        return static_cast<size_t>(y_cell * width_ + x);
    }

public:
    PixelCanvas() = default;

    PixelCanvas(int width, int height)
        : width_(width), height_(height),
          top_pixels_(static_cast<size_t>(width * height), Color::black()),
          bottom_pixels_(static_cast<size_t>(width * height), Color::black()) {}

    // -- Accessors --
    [[nodiscard]] int width()        const noexcept { return width_; }
    [[nodiscard]] int height()       const noexcept { return height_; }
    [[nodiscard]] int pixel_height() const noexcept { return height_ * 2; }

    // -- Pixel manipulation --
    // x: [0, width_), y: [0, height_*2)
    void set_pixel(int x, int y, Color color) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_ * 2) return;
        int cell_y = y / 2;
        if (y % 2 == 0) {
            top_pixels_[idx(x, cell_y)] = color;
        } else {
            bottom_pixels_[idx(x, cell_y)] = color;
        }
    }

    void clear() {
        std::fill(top_pixels_.begin(), top_pixels_.end(), bg_);
        std::fill(bottom_pixels_.begin(), bottom_pixels_.end(), bg_);
    }

    void fill(Color color) {
        std::fill(top_pixels_.begin(), top_pixels_.end(), color);
        std::fill(bottom_pixels_.begin(), bottom_pixels_.end(), color);
    }

    // -- Drawing primitives --
    void line(int x1, int y1, int x2, int y2, Color color) {
        // Bresenham's line algorithm
        int dx = std::abs(x2 - x1);
        int dy = std::abs(y2 - y1);
        int sx = (x1 < x2) ? 1 : -1;
        int sy = (y1 < y2) ? 1 : -1;
        int err = dx - dy;

        while (true) {
            set_pixel(x1, y1, color);
            if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 <  dx) { err += dx; y1 += sy; }
        }
    }

    void rect(int x, int y, int w, int h, Color color) {
        // Draw outline rectangle
        for (int i = x; i < x + w; ++i) {
            set_pixel(i, y, color);
            set_pixel(i, y + h - 1, color);
        }
        for (int j = y; j < y + h; ++j) {
            set_pixel(x, j, color);
            set_pixel(x + w - 1, j, color);
        }
    }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (width_ == 0 || height_ == 0) {
            return Element{TextElement{}};
        }

        // Each row is one line of half-block characters.
        // We use StyledRuns to color each cell individually.
        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(height_));

        for (int cy = 0; cy < height_; ++cy) {
            std::string content;
            std::vector<StyledRun> runs;
            content.reserve(static_cast<size_t>(width_ * 3));  // up to 3 bytes per char

            for (int cx = 0; cx < width_; ++cx) {
                Color top = top_pixels_[idx(cx, cy)];
                Color bot = bottom_pixels_[idx(cx, cy)];

                size_t start = content.size();

                // Upper half block: fg = top color, bg = bottom color
                content += "\xe2\x96\x80";  // U+2580 "▀"

                Style s = Style{}.with_fg(top).with_bg(bot);
                runs.push_back(StyledRun{start, 3, s});
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
