#pragma once
// maya::widget::image — Braille-based image renderer
//
// Converts a boolean pixel grid into Unicode braille characters (U+2800-U+28FF).
// Each terminal cell maps to a 2x4 dot matrix, giving 2x horizontal and 4x
// vertical sub-cell resolution.
//
//   ⣿⣿⣿⣿⣿⣿⣿⣿
//   ⣿⠀⠀⠀⠀⠀⠀⣿
//   ⣿⣿⣿⣿⣿⣿⣿⣿
//
// Usage:
//   Image img(16, 16);
//   img.set_pixel(0, 0, true);
//   img.line(0, 0, 15, 15);
//   auto ui = img.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

class Image {
    int width_  = 0;  // pixel width
    int height_ = 0;  // pixel height
    std::vector<bool> pixels_;
    Color color_ = Color::white();

    // Braille dot positions within a 2x4 cell:
    //   (0,0)=0x01  (1,0)=0x08
    //   (0,1)=0x02  (1,1)=0x10
    //   (0,2)=0x04  (1,2)=0x20
    //   (0,3)=0x40  (1,3)=0x80
    static constexpr uint8_t dot_map[4][2] = {
        {0x01, 0x08},
        {0x02, 0x10},
        {0x04, 0x20},
        {0x40, 0x80},
    };

    [[nodiscard]] bool get_pixel(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;
        return pixels_[static_cast<size_t>(y * width_ + x)];
    }

    // Encode a single braille codepoint (U+2800 + offset) as UTF-8
    static void encode_braille(uint8_t bits, std::string& out) {
        // U+2800 = 0xE2 0xA0 0x80 in UTF-8
        // Braille range: U+2800..U+28FF (codepoint = 0x2800 + bits)
        char32_t cp = 0x2800 + bits;
        // 3-byte UTF-8 encoding for U+0800..U+FFFF
        out += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }

public:
    Image() = default;

    Image(int width, int height, Color color = Color::white())
        : width_(width), height_(height),
          pixels_(static_cast<size_t>(width * height), false),
          color_(color) {}

    // -- Accessors --
    [[nodiscard]] int width()  const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // -- Mutation --
    void set_pixel(int x, int y, bool on = true) {
        if (x >= 0 && x < width_ && y >= 0 && y < height_)
            pixels_[static_cast<size_t>(y * width_ + x)] = on;
    }

    void clear() {
        std::fill(pixels_.begin(), pixels_.end(), false);
    }

    void set_color(Color c) { color_ = c; }

    /// Create an image from ASCII art. '#' = on, anything else = off.
    [[nodiscard]] static Image from_text(std::string_view ascii_art) {
        // Parse lines
        std::vector<std::string_view> lines;
        int max_w = 0;
        while (!ascii_art.empty()) {
            auto nl = ascii_art.find('\n');
            auto line = (nl == std::string_view::npos) ? ascii_art : ascii_art.substr(0, nl);
            lines.push_back(line);
            max_w = std::max(max_w, static_cast<int>(line.size()));
            ascii_art = (nl == std::string_view::npos)
                ? std::string_view{}
                : ascii_art.substr(nl + 1);
        }

        Image img(max_w, static_cast<int>(lines.size()));
        for (int y = 0; y < static_cast<int>(lines.size()); ++y) {
            for (int x = 0; x < static_cast<int>(lines[static_cast<size_t>(y)].size()); ++x) {
                if (lines[static_cast<size_t>(y)][static_cast<size_t>(x)] == '#') {
                    img.set_pixel(x, y, true);
                }
            }
        }
        return img;
    }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (width_ == 0 || height_ == 0) {
            return Element{TextElement{}};
        }

        // Each braille cell covers 2 pixels wide, 4 pixels tall
        int cells_x = (width_ + 1) / 2;
        int cells_y = (height_ + 3) / 4;

        std::string content;
        content.reserve(static_cast<size_t>(cells_x * cells_y * 3 + cells_y));  // 3 bytes per braille + newlines

        for (int cy = 0; cy < cells_y; ++cy) {
            if (cy > 0) content += '\n';
            for (int cx = 0; cx < cells_x; ++cx) {
                uint8_t bits = 0;
                int px = cx * 2;
                int py = cy * 4;
                for (int dy = 0; dy < 4; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        if (get_pixel(px + dx, py + dy)) {
                            bits |= dot_map[dy][dx];
                        }
                    }
                }
                encode_braille(bits, content);
            }
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = Style{}.with_fg(color_),
        }};
    }
};

} // namespace maya
