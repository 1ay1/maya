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

    explicit AlignedBuffer(std::size_t count, uint64_t fill_value = 0);

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

    void resize(std::size_t count, uint64_t fill_value = 0);
    void assign(std::size_t count, uint64_t value);

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
    StylePool();

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
    void clear();

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
    static char* write_uint_sgr(char* p, unsigned n) noexcept;
    static char* append_color_sgr(char* p, const Color& c, bool is_fg) noexcept;
    static std::string build_sgr(const Style& s);

    /// FNV-1a hash over the style's packed representation.
    [[nodiscard]] static std::size_t hash_style(const Style& s) noexcept;

    void insert_slot(std::size_t h, uint16_t id);
    void grow(std::size_t new_cap);
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

    Canvas(int width, int height, StylePool* pool);

    // -- Cell access ----------------------------------------------------------

    /// Set a cell at (x, y). Clipped or out-of-bounds coordinates are silently ignored.
    [[gnu::always_inline]] void set(int x, int y, char32_t ch, uint16_t style_id, uint8_t width = 0) {
        if (__builtin_expect(!in_bounds(x, y), 0)) return;
        if (!clip_stack_.empty() && __builtin_expect(!clip_stack_.back().contains({Columns{x}, Rows{y}}), 0)) return;

        auto idx = cell_index(x, y);
        uint64_t packed = Cell{ch, style_id, 0, width}.pack();
        cells_[idx] = packed;  // unconditional write; diff will skip unchanged cells
    }

    /// Read the cell at (x, y). Out-of-bounds returns a default cell.
    [[nodiscard]] Cell get(int x, int y) const noexcept;

    /// Direct access to the packed cell value at (x, y) for fast diff.
    [[gnu::always_inline]] [[nodiscard]] uint64_t get_packed(int x, int y) const noexcept {
        return cells_[static_cast<std::size_t>(y * width_ + x)];
    }

    // -- Text rendering -------------------------------------------------------

    /// Write a UTF-8 string starting at (x, y).
    void write_text(int x, int y, std::string_view text, uint16_t style_id);

    // -- Region operations ----------------------------------------------------

    /// Fill a rectangular region with a character and style.
    void fill(Rect region, char32_t ch, uint16_t style_id);

    /// Reset all cells to space with the default style. Clears damage.
    void clear();

    // -- Clip stack -----------------------------------------------------------

    /// Push a clipping rectangle.
    void push_clip(Rect clip);

    /// Pop the most recent clipping rectangle.
    void pop_clip();

    /// Returns true if (x, y) falls outside the current clip region.
    [[nodiscard]] bool is_clipped(int x, int y) const noexcept;

    // -- Sizing ---------------------------------------------------------------

    /// Resize the canvas, clearing all content.
    void resize(int w, int h);

    [[nodiscard]] int width()  const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // -- Damage tracking ------------------------------------------------------

    void mark_damage(Rect region);
    [[nodiscard]] Rect damage() const noexcept { return damage_; }
    void reset_damage() noexcept { damage_ = Rect{}; }
    void mark_all_damaged();

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
