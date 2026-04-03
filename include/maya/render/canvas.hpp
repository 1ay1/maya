#pragma once
// maya::render::canvas - Double-buffered 2D cell grid for terminal rendering
//
// Canvas is the rendering surface: a width x height grid of packed 64-bit
// cells. Each cell holds a Unicode code point, a style ID (interned via
// StylePool), a hyperlink ID, and a width marker for CJK/wide characters.
//
// The 64-bit packing enables O(1) cell comparison in the diff algorithm --
// two cells are identical iff their packed representations are equal.
// Dirty-region tracking (damage rect) limits the diff to only the changed
// area, and a clip stack supports overflow:hidden containers.

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../core/types.hpp"
#include "../style/style.hpp"
#include "../element/text.hpp"

namespace maya {

// ============================================================================
// Cell - A single terminal cell (packed into 64 bits)
// ============================================================================
// Memory layout (8 bytes total):
//   [0..3]  char32_t character   (32 bits)
//   [4..5]  uint16_t style_id    (16 bits - index into StylePool)
//   [6..7]  uint8_t  hyperlink_id high byte, width low byte
//
// The pack/unpack scheme is designed so that identical cells produce
// identical 64-bit values, enabling the diff to compare cells with a
// single integer comparison.

struct Cell {
    char32_t character    = U' ';
    uint16_t style_id     = 0;
    uint16_t hyperlink_id = 0;
    uint8_t  width        = 0;  // 0 = normal, 1 = wide first half, 2 = wide second half

    /// Pack all fields into a single 64-bit integer for fast comparison.
    [[nodiscard]] constexpr uint64_t pack() const noexcept {
        return static_cast<uint64_t>(character)
             | (static_cast<uint64_t>(style_id)     << 32)
             | (static_cast<uint64_t>(hyperlink_id) << 48)
             | (static_cast<uint64_t>(width)        << 56);  // top 8 bits unused except 3
    }

    /// Reconstruct a Cell from its packed 64-bit representation.
    [[nodiscard]] static constexpr Cell unpack(uint64_t packed) noexcept {
        return Cell{
            .character    = static_cast<char32_t>(packed & 0xFFFFFFFF),
            .style_id     = static_cast<uint16_t>((packed >> 32) & 0xFFFF),
            .hyperlink_id = static_cast<uint16_t>((packed >> 48) & 0xFF),
            .width        = static_cast<uint8_t>((packed >> 56) & 0xFF),
        };
    }

    constexpr bool operator==(const Cell&) const = default;
};

// Verify packing round-trips correctly.
static_assert(Cell{U'A', 42, 7, 1}.pack() != 0);
static_assert(Cell::unpack(Cell{U'A', 42, 7, 1}.pack()) == Cell{U'A', 42, 7, 1});
static_assert(Cell{}.pack() == Cell{U' ', 0, 0, 0}.pack());

// ============================================================================
// StylePool - Interning pool for Style objects
// ============================================================================
// Assigns each unique Style a compact uint16_t ID. Identical styles always
// receive the same ID, so the canvas stores only 16-bit indices instead of
// full Style objects. This makes cell packing feasible and the diff cheap.
//
// The pool is shared between front and back canvases in the double-buffer
// system so that style IDs are comparable across frames.

class StylePool {
public:
    StylePool() {
        // ID 0 is always the default (empty) style.
        styles_.emplace_back();
        map_[Style{}] = 0;
    }

    /// Intern a style, returning its unique ID. If the style already
    /// exists in the pool, the existing ID is returned.
    [[nodiscard]] uint16_t intern(const Style& s) {
        auto it = map_.find(s);
        if (it != map_.end()) return it->second;

        auto id = static_cast<uint16_t>(styles_.size());
        styles_.push_back(s);
        map_[s] = id;
        return id;
    }

    /// Look up a style by its ID. The caller must ensure the ID is valid.
    [[nodiscard]] const Style& get(uint16_t id) const noexcept {
        return styles_[id];
    }

    /// Number of interned styles.
    [[nodiscard]] std::size_t size() const noexcept {
        return styles_.size();
    }

    /// Reset the pool back to only the default style.
    void clear() {
        styles_.resize(1);
        map_.clear();
        map_[Style{}] = 0;
    }

private:
    // We need a hash for Style to use it as an unordered_map key.
    struct StyleHash {
        std::size_t operator()(const Style& s) const noexcept {
            // FNV-1a inspired hash over the style's boolean flags and color values.
            std::size_t h = 14695981039346656037ULL;
            auto mix = [&](uint64_t v) {
                h ^= v;
                h *= 1099511628211ULL;
            };

            uint8_t flags = 0;
            if (s.bold)          flags |= (1 << 0);
            if (s.dim)           flags |= (1 << 1);
            if (s.italic)        flags |= (1 << 2);
            if (s.underline)     flags |= (1 << 3);
            if (s.strikethrough) flags |= (1 << 4);
            if (s.inverse)       flags |= (1 << 5);
            mix(flags);

            if (s.fg.has_value()) {
                mix(static_cast<uint64_t>(s.fg->kind()) << 24
                  | static_cast<uint64_t>(s.fg->r()) << 16
                  | static_cast<uint64_t>(s.fg->g()) << 8
                  | static_cast<uint64_t>(s.fg->b()));
            } else {
                mix(0xDEAD);
            }

            if (s.bg.has_value()) {
                mix(static_cast<uint64_t>(s.bg->kind()) << 24
                  | static_cast<uint64_t>(s.bg->r()) << 16
                  | static_cast<uint64_t>(s.bg->g()) << 8
                  | static_cast<uint64_t>(s.bg->b()));
            } else {
                mix(0xBEEF);
            }

            return h;
        }
    };

    struct StyleEqual {
        bool operator()(const Style& a, const Style& b) const noexcept {
            return a == b;
        }
    };

    std::vector<Style> styles_;
    std::unordered_map<Style, uint16_t, StyleHash, StyleEqual> map_;
};

// ============================================================================
// Canvas - A 2D grid of packed cells
// ============================================================================
// The core rendering surface. Two canvases (front and back) form the
// double-buffer system. Elements render into the back canvas; the diff
// algorithm compares back vs front to produce minimal terminal output.
//
// All coordinate systems are 0-based. The origin (0,0) is the top-left
// corner of the canvas.

class Canvas {
public:
    Canvas() = default;

    Canvas(int width, int height, StylePool* pool)
        : width_(width)
        , height_(height)
        , style_pool_(pool)
    {
        cells_.resize(static_cast<std::size_t>(width_ * height_), default_cell());
    }

    // -- Cell access ----------------------------------------------------------

    /// Set a cell at (x, y). Clipped coordinates are silently ignored.
    void set(int x, int y, char32_t ch, uint16_t style_id, uint8_t width = 0) {
        if (is_clipped(x, y)) return;
        if (!in_bounds(x, y)) return;

        auto idx = cell_index(x, y);
        uint64_t packed = Cell{ch, style_id, 0, width}.pack();
        if (cells_[idx] != packed) {
            cells_[idx] = packed;
            mark_damage(Rect{{Columns{x}, Rows{y}}, {Columns{1}, Rows{1}}});
        }
    }

    /// Read the cell at (x, y). Out-of-bounds returns a default cell.
    [[nodiscard]] Cell get(int x, int y) const noexcept {
        if (!in_bounds(x, y)) return Cell{};
        return Cell::unpack(cells_[cell_index(x, y)]);
    }

    /// Direct access to the packed cell value at (x, y) for fast diff.
    [[nodiscard]] uint64_t get_packed(int x, int y) const noexcept {
        if (!in_bounds(x, y)) return Cell{}.pack();
        return cells_[cell_index(x, y)];
    }

    // -- Text rendering -------------------------------------------------------

    /// Write a UTF-8 string starting at (x, y). Handles wide characters
    /// by writing a wide-first-half cell followed by a wide-second-half
    /// placeholder. Advances x by the display width of each character.
    void write_text(int x, int y, std::string_view text, uint16_t style_id) {
        int cx = x;
        std::size_t pos = 0;
        while (pos < text.size()) {
            char32_t cp = decode_utf8(text, pos);
            if (cp < 0x20) continue;  // skip control characters

            if (is_wide_char(cp)) {
                set(cx, y, cp, style_id, 1);       // first half
                set(cx + 1, y, U' ', style_id, 2); // second half (placeholder)
                cx += 2;
            } else {
                set(cx, y, cp, style_id, 0);
                cx += 1;
            }
        }
    }

    // -- Region operations ----------------------------------------------------

    /// Fill a rectangular region with a character and style.
    void fill(Rect region, char32_t ch, uint16_t style_id) {
        int x0 = std::max(0, region.left().value);
        int y0 = std::max(0, region.top().value);
        int x1 = std::min(width_, region.right().value);
        int y1 = std::min(height_, region.bottom().value);

        uint64_t packed = Cell{ch, style_id, 0, 0}.pack();

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (is_clipped(x, y)) continue;
                auto idx = cell_index(x, y);
                if (cells_[idx] != packed) {
                    cells_[idx] = packed;
                }
            }
        }

        // Mark the entire fill region as damaged.
        mark_damage(Rect{
            {Columns{x0}, Rows{y0}},
            {Columns{x1 - x0}, Rows{y1 - y0}}
        });
    }

    /// Reset all cells to space with the default style. Clears damage.
    void clear() {
        uint64_t blank = default_cell();
        std::fill(cells_.begin(), cells_.end(), blank);
        damage_ = full_rect();
    }

    // -- Clip stack -----------------------------------------------------------

    /// Push a clipping rectangle. Subsequent writes outside this rect
    /// (intersected with all parent clips) are silently discarded.
    void push_clip(Rect clip) {
        if (clip_stack_.empty()) {
            clip_stack_.push_back(clip);
        } else {
            // Intersect with the current effective clip.
            clip_stack_.push_back(clip_stack_.back().intersect(clip));
        }
    }

    /// Pop the most recent clipping rectangle.
    void pop_clip() {
        if (!clip_stack_.empty()) {
            clip_stack_.pop_back();
        }
    }

    /// Returns true if (x, y) falls outside the current clip region.
    [[nodiscard]] bool is_clipped(int x, int y) const noexcept {
        if (clip_stack_.empty()) return false;
        return !clip_stack_.back().contains({Columns{x}, Rows{y}});
    }

    // -- Sizing ---------------------------------------------------------------

    /// Resize the canvas, clearing all content.
    void resize(int w, int h) {
        width_ = w;
        height_ = h;
        cells_.assign(static_cast<std::size_t>(w * h), default_cell());
        damage_ = full_rect();
        clip_stack_.clear();
    }

    [[nodiscard]] int width()  const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // -- Damage tracking ------------------------------------------------------

    /// Expand the damage region to include the given rectangle.
    void mark_damage(Rect region) {
        damage_ = damage_.unite(region);
    }

    /// Return the current damage rectangle (the union of all dirty regions).
    [[nodiscard]] Rect damage() const noexcept { return damage_; }

    /// Reset the damage region to empty.
    void reset_damage() noexcept {
        damage_ = Rect{};
    }

    /// Mark the entire canvas as damaged.
    void mark_all_damaged() {
        damage_ = full_rect();
    }

    // -- Style pool access ----------------------------------------------------

    [[nodiscard]] StylePool* style_pool() const noexcept { return style_pool_; }
    void set_style_pool(StylePool* pool) noexcept { style_pool_ = pool; }

    // -- Raw buffer access (for diff) -----------------------------------------

    [[nodiscard]] const std::vector<uint64_t>& cells() const noexcept { return cells_; }

private:
    [[nodiscard]] bool in_bounds(int x, int y) const noexcept {
        return x >= 0 && x < width_ && y >= 0 && y < height_;
    }

    [[nodiscard]] std::size_t cell_index(int x, int y) const noexcept {
        return static_cast<std::size_t>(y * width_ + x);
    }

    [[nodiscard]] static uint64_t default_cell() noexcept {
        return Cell{U' ', 0, 0, 0}.pack();
    }

    [[nodiscard]] Rect full_rect() const noexcept {
        return {{Columns{0}, Rows{0}}, {Columns{width_}, Rows{height_}}};
    }

    std::vector<uint64_t> cells_;
    int width_  = 0;
    int height_ = 0;
    StylePool* style_pool_ = nullptr;
    Rect damage_{};
    std::vector<Rect> clip_stack_;
};

} // namespace maya
