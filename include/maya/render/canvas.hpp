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
#include <new>
#include <string_view>
#include <vector>

#include "../core/types.hpp"
#include "../core/simd.hpp"
#include "../style/style.hpp"
#include "../element/text.hpp"

namespace maya {

// ============================================================================
// AlignedBuffer - 64-byte cache-line aligned buffer for SIMD
// ============================================================================
// AVX2 loads from 32-byte aligned addresses avoid split cache-line penalties.
// We align to 64 bytes (full cache line) so that SIMD diff loops hit aligned
// addresses more often. This replaces std::vector<uint64_t> for cell storage.

class AlignedBuffer {
public:
    AlignedBuffer() = default;

    explicit AlignedBuffer(std::size_t count, uint64_t fill_value = 0)
        : size_(count)
        , capacity_(count)
    {
        if (count > 0) {
            data_ = static_cast<uint64_t*>(
                ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
            std::fill(data_, data_ + count, fill_value);
        }
    }

    ~AlignedBuffer() { free(); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& o) noexcept
        : data_(o.data_), size_(o.size_), capacity_(o.capacity_)
    { o.data_ = nullptr; o.size_ = o.capacity_ = 0; }

    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            free();
            data_ = o.data_; size_ = o.size_; capacity_ = o.capacity_;
            o.data_ = nullptr; o.size_ = o.capacity_ = 0;
        }
        return *this;
    }

    void resize(std::size_t count, uint64_t fill_value = 0) {
        if (count <= capacity_) {
            if (count > size_) std::fill(data_ + size_, data_ + count, fill_value);
            size_ = count;
            return;
        }
        auto* new_data = static_cast<uint64_t*>(
            ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
        if (data_) {
            std::copy(data_, data_ + size_, new_data);
            free();
        }
        std::fill(new_data + size_, new_data + count, fill_value);
        data_ = new_data;
        size_ = capacity_ = count;
    }

    void assign(std::size_t count, uint64_t value) {
        if (count > capacity_) {
            free();
            data_ = static_cast<uint64_t*>(
                ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
            capacity_ = count;
        }
        size_ = count;
        simd::streaming_fill(data_, count, value);
    }

    [[nodiscard]] uint64_t* data() noexcept { return data_; }
    [[nodiscard]] const uint64_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    uint64_t& operator[](std::size_t i) noexcept { return data_[i]; }
    const uint64_t& operator[](std::size_t i) const noexcept { return data_[i]; }

    uint64_t* begin() noexcept { return data_; }
    uint64_t* end() noexcept { return data_ + size_; }
    const uint64_t* begin() const noexcept { return data_; }
    const uint64_t* end() const noexcept { return data_ + size_; }

private:
    void free() {
        if (data_) ::operator delete(data_, std::align_val_t{64});
        data_ = nullptr;
    }

    uint64_t*   data_     = nullptr;
    std::size_t size_     = 0;
    std::size_t capacity_ = 0;
};

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

// ============================================================================
// StylePool - Open-addressing flat hash map for style interning
// ============================================================================
// Replaces std::unordered_map with a cache-friendly open-addressing table.
// Linear probing on a power-of-2 table gives O(1) amortized lookup with
// zero pointer chasing. For typical TUI workloads (10-50 unique styles),
// this is ~5x faster than std::unordered_map.
//
// Layout:
//   styles_[]  — flat vector of Style objects, indexed by uint16_t ID
//   slots_[]   — open-addressing table: { hash, id } pairs
//   hash == 0  — empty sentinel (real hashes that collide with 0 are mapped to 1)

class StylePool {
public:
    StylePool() {
        styles_.reserve(64);
        sgr_cache_.reserve(64);
        styles_.emplace_back();  // ID 0 = default (empty) style
        sgr_cache_.push_back(build_sgr(styles_[0]));
        grow(64);
        // Insert the default style into the map.
        insert_slot(hash_style(styles_[0]), 0);
        size_ = 1;
    }

    /// Intern a style, returning its unique ID.
    /// Hot path: called once per cell during painting.
    [[gnu::always_inline]] [[nodiscard]] uint16_t intern(const Style& s) {
        std::size_t h = hash_style(s);
        std::size_t idx = h & mask_;

        while (true) {
            auto& slot = slots_[idx];
            if (slot.hash == 0) [[unlikely]] {
                // Empty slot — insert new style.
                auto id = static_cast<uint16_t>(styles_.size());
                styles_.push_back(s);
                sgr_cache_.push_back(build_sgr(s));
                slot = {h, id};
                ++size_;
                if (size_ * 4 > capacity_ * 3) [[unlikely]] {
                    grow(capacity_ * 2);
                }
                return id;
            }
            if (slot.hash == h && styles_[slot.id] == s) [[likely]] {
                return slot.id;
            }
            idx = (idx + 1) & mask_;
        }
    }

    /// Look up a style by its ID. The caller must ensure the ID is valid.
    [[gnu::always_inline]] [[nodiscard]] const Style& get(uint16_t id) const noexcept {
        return styles_[id];
    }

    /// Pre-built SGR escape sequence for a style ID.
    /// Each string is a complete "\x1b[0;...m" that resets then applies — the diff
    /// loop calls this once per style change instead of computing transitions.
    [[gnu::always_inline]] [[nodiscard]] std::string_view sgr(uint16_t id) const noexcept {
        return sgr_cache_[id];
    }

    /// Number of interned styles.
    [[nodiscard]] std::size_t size() const noexcept { return styles_.size(); }

    /// Reset the pool back to only the default style.
    void clear() {
        styles_.resize(1);
        sgr_cache_.resize(1);
        size_ = 1;
        std::fill(slots_.begin(), slots_.end(), Slot{});
        insert_slot(hash_style(styles_[0]), 0);
    }

private:
    struct Slot {
        std::size_t hash = 0;  // 0 = empty sentinel
        uint16_t    id   = 0;
    };

    std::vector<Style>       styles_;
    std::vector<std::string> sgr_cache_;  // sgr_cache_[id] = pre-built "\x1b[0;...m"
    std::vector<Slot>        slots_;
    std::size_t size_     = 0;
    std::size_t capacity_ = 0;
    std::size_t mask_     = 0;

    // ── SGR cache builder ────────────────────────────────────────────────
    // Builds the complete ANSI SGR escape sequence for a style, including
    // an implicit reset (parameter 0). The diff loop emits this verbatim
    // instead of computing style transitions at runtime.
    //
    // Format: \x1b[0{;attr}*{;fg}{;bg}m
    // Max:    \x1b[0;1;2;3;4;7;9;38;2;255;255;255;48;2;255;255;255m  (~50 bytes)

    static char* write_uint_sgr(char* p, unsigned n) noexcept {
        if (n < 10) {
            *p++ = static_cast<char>('0' + n);
        } else if (n < 100) {
            *p++ = static_cast<char>('0' + n / 10);
            *p++ = static_cast<char>('0' + n % 10);
        } else {
            *p++ = static_cast<char>('0' + n / 100);
            *p++ = static_cast<char>('0' + (n / 10) % 10);
            *p++ = static_cast<char>('0' + n % 10);
        }
        return p;
    }

    static char* append_color_sgr(char* p, const Color& c, bool is_fg) noexcept {
        switch (c.kind()) {
            case Color::Kind::Named: {
                int base = is_fg ? 30 : 40;
                int code = c.r() < 8 ? base + c.r() : (base + 60) + (c.r() - 8);
                return write_uint_sgr(p, static_cast<unsigned>(code));
            }
            case Color::Kind::Indexed:
                if (is_fg) { *p++='3'; *p++='8'; } else { *p++='4'; *p++='8'; }
                *p++ = ';'; *p++ = '5'; *p++ = ';';
                return write_uint_sgr(p, c.r());
            case Color::Kind::Rgb:
                if (is_fg) { *p++='3'; *p++='8'; } else { *p++='4'; *p++='8'; }
                *p++ = ';'; *p++ = '2'; *p++ = ';';
                p = write_uint_sgr(p, c.r()); *p++ = ';';
                p = write_uint_sgr(p, c.g()); *p++ = ';';
                return write_uint_sgr(p, c.b());
        }
        __builtin_unreachable();
    }

    static std::string build_sgr(const Style& s) {
        char buf[64];
        char* p = buf;
        *p++ = '\x1b'; *p++ = '['; *p++ = '0';

        if (s.bold)          { *p++ = ';'; *p++ = '1'; }
        if (s.dim)           { *p++ = ';'; *p++ = '2'; }
        if (s.italic)        { *p++ = ';'; *p++ = '3'; }
        if (s.underline)     { *p++ = ';'; *p++ = '4'; }
        if (s.inverse)       { *p++ = ';'; *p++ = '7'; }
        if (s.strikethrough) { *p++ = ';'; *p++ = '9'; }

        if (s.fg.has_value()) { *p++ = ';'; p = append_color_sgr(p, *s.fg, true); }
        if (s.bg.has_value()) { *p++ = ';'; p = append_color_sgr(p, *s.bg, false); }

        *p++ = 'm';
        return {buf, static_cast<std::size_t>(p - buf)};
    }

    /// FNV-1a hash over the style's packed representation.
    [[nodiscard]] static std::size_t hash_style(const Style& s) noexcept {
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

        // Map hash 0 → 1 (reserve 0 as empty sentinel).
        return h == 0 ? 1 : h;
    }

    void insert_slot(std::size_t h, uint16_t id) {
        std::size_t idx = h & mask_;
        while (slots_[idx].hash != 0) {
            idx = (idx + 1) & mask_;
        }
        slots_[idx] = {h, id};
    }

    void grow(std::size_t new_cap) {
        capacity_ = new_cap;
        mask_ = new_cap - 1;
        slots_.assign(new_cap, Slot{});
        // Re-insert all existing styles.
        for (uint16_t i = 0; i < static_cast<uint16_t>(styles_.size()); ++i) {
            insert_slot(hash_style(styles_[i]), i);
        }
    }
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

    /// Set a cell at (x, y). Clipped or out-of-bounds coordinates are silently ignored.
    /// mark_damage() is intentionally omitted here: the renderer always calls clear()
    /// before painting, which marks the full canvas as damaged. Per-cell damage
    /// tracking is therefore redundant and only wastes cycles.
    [[gnu::always_inline]] void set(int x, int y, char32_t ch, uint16_t style_id, uint8_t width = 0) {
        if (__builtin_expect(!in_bounds(x, y), 0)) return;
        if (!clip_stack_.empty() && __builtin_expect(!clip_stack_.back().contains({Columns{x}, Rows{y}}), 0)) return;

        auto idx = cell_index(x, y);
        uint64_t packed = Cell{ch, style_id, 0, width}.pack();
        cells_[idx] = packed;  // unconditional write; diff will skip unchanged cells
    }

    /// Read the cell at (x, y). Out-of-bounds returns a default cell.
    [[nodiscard]] Cell get(int x, int y) const noexcept {
        if (!in_bounds(x, y)) return Cell{};
        return Cell::unpack(cells_[cell_index(x, y)]);
    }

    /// Direct access to the packed cell value at (x, y) for fast diff.
    [[gnu::always_inline]] [[nodiscard]] uint64_t get_packed(int x, int y) const noexcept {
        return cells_[static_cast<std::size_t>(y * width_ + x)];
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
    /// Hot path: uses row-wise std::fill on contiguous memory segments instead of
    /// per-pixel is_clipped() checks. Clip intersection is computed once upfront.
    void fill(Rect region, char32_t ch, uint16_t style_id) {
        int x0 = std::max(0, region.left().value);
        int y0 = std::max(0, region.top().value);
        int x1 = std::min(width_, region.right().value);
        int y1 = std::min(height_, region.bottom().value);

        // Apply active clip in one shot — no per-pixel check needed.
        if (!clip_stack_.empty()) {
            const Rect& clip = clip_stack_.back();
            x0 = std::max(x0, clip.left().value);
            y0 = std::max(y0, clip.top().value);
            x1 = std::min(x1, clip.right().value);
            y1 = std::min(y1, clip.bottom().value);
        }

        if (x0 >= x1 || y0 >= y1) return;

        uint64_t packed = Cell{ch, style_id, 0, 0}.pack();
        uint64_t* base  = cells_.data();

        for (int y = y0; y < y1; ++y) {
            std::fill(base + y * width_ + x0, base + y * width_ + x1, packed);
        }
    }

    /// Reset all cells to space with the default style. Clears damage.
    void clear() {
        uint64_t blank = default_cell();
        simd::streaming_fill(cells_.data(), cells_.size(), blank);
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

    [[nodiscard]] const uint64_t* cells() const noexcept { return cells_.data(); }
    [[nodiscard]] std::size_t cell_count() const noexcept { return cells_.size(); }

private:
    [[gnu::always_inline]] [[nodiscard]] bool in_bounds(int x, int y) const noexcept {
        return static_cast<unsigned>(x) < static_cast<unsigned>(width_)
            && static_cast<unsigned>(y) < static_cast<unsigned>(height_);
    }

    [[gnu::always_inline]] [[nodiscard]] std::size_t cell_index(int x, int y) const noexcept {
        return static_cast<std::size_t>(y * width_ + x);
    }

    [[nodiscard]] static uint64_t default_cell() noexcept {
        return Cell{U' ', 0, 0, 0}.pack();
    }

    [[nodiscard]] Rect full_rect() const noexcept {
        return {{Columns{0}, Rows{0}}, {Columns{width_}, Rows{height_}}};
    }

    AlignedBuffer cells_;
    int width_  = 0;
    int height_ = 0;
    StylePool* style_pool_ = nullptr;
    Rect damage_{};
    std::vector<Rect> clip_stack_;
};

} // namespace maya
