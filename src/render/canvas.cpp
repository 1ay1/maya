#include "maya/render/canvas.hpp"

#include <algorithm>
#include <atomic>
#include <new>

namespace maya {

// ============================================================================
// AlignedBuffer
// ============================================================================

AlignedBuffer::AlignedBuffer(std::size_t count, uint64_t fill_value)
    : size_(count)
    , capacity_(count)
{
    if (count > 0) {
        data_ = static_cast<uint64_t*>(
            ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
        std::fill(data_, data_ + count, fill_value);
    }
}

void AlignedBuffer::resize(std::size_t count, uint64_t fill_value) {
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

void AlignedBuffer::assign(std::size_t count, uint64_t value) {
    if (count > capacity_) {
        free();
        data_ = static_cast<uint64_t*>(
            ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
        capacity_ = count;
    }
    size_ = count;
    simd::streaming_fill(data_, count, value);
}

// ============================================================================
// StylePool
// ============================================================================

// Process-wide monotone counter for StylePool identity. Starts at 1 so
// `pool_id_ == 0` is a recognisable "uninitialised" sentinel for any
// thread-local cache that needs one.
namespace {
std::atomic<uint64_t> g_next_pool_id{1};
}

StylePool::StylePool() {
    pool_id_ = g_next_pool_id.fetch_add(1, std::memory_order_relaxed);
    styles_.reserve(64);
    sgr_cache_.reserve(64);
    styles_.emplace_back();  // ID 0 = default (empty) style
    sgr_cache_.push_back(build_sgr(styles_[0]));
    grow(64);
    // Insert the default style into the map.
    insert_slot(hash_style(styles_[0]), 0);
    size_ = 1;
}

void StylePool::clear() {
    // Bump pool_id_ so any thread_local intern_const cache that
    // recorded the old id sees a mismatch on next lookup and
    // re-interns. Without this, calling clear() leaves stale
    // intern_const cache slots pointing to ids that were just dropped.
    pool_id_ = g_next_pool_id.fetch_add(1, std::memory_order_relaxed);
    styles_.resize(1);
    sgr_cache_.resize(1);
    size_ = 1;
    std::fill(slots_.begin(), slots_.end(), Slot{});
    insert_slot(hash_style(styles_[0]), 0);
}

char* StylePool::write_uint_sgr(char* p, unsigned n) noexcept {
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

char* StylePool::append_color_sgr(char* p, const Color& c, bool is_fg) noexcept {
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

void StylePool::write_transition_sgr(uint16_t prev_id, uint16_t new_id,
                                     std::string& out) const {
    if (prev_id == new_id) return;

    // Unknown terminal state (no SGR emitted yet this frame). Fall back
    // to the full reset-and-set form so the terminal lands at a known
    // configuration regardless of any residual SGR from before maya
    // took over the screen.
    if (prev_id == UINT16_MAX) {
        out.append(sgr_cache_[new_id]);
        return;
    }

    const Style& from = styles_[prev_id];
    const Style& to   = styles_[new_id];

    char buf[96];
    char* p = buf;
    *p++ = '\x1b'; *p++ = '[';
    bool first = true;
    auto sep = [&]() noexcept { if (!first) *p++ = ';'; first = false; };

    // Attribute toggles. SGR 22 = bold-off (per spec, also turns off
    // dim; we suppress dim emission entirely so the conflation is
    // moot here — see build_sgr for the SGR-2 suppression rationale).
    if (from.bold != to.bold) {
        sep();
        if (to.bold) *p++ = '1';
        else        { *p++ = '2'; *p++ = '2'; }
    }
    if (from.italic != to.italic) {
        sep();
        if (to.italic) *p++ = '3';
        else          { *p++ = '2'; *p++ = '3'; }
    }
    if (from.underline != to.underline) {
        sep();
        if (to.underline) *p++ = '4';
        else             { *p++ = '2'; *p++ = '4'; }
    }
    if (from.inverse != to.inverse) {
        sep();
        if (to.inverse) *p++ = '7';
        else           { *p++ = '2'; *p++ = '7'; }
    }
    if (from.strikethrough != to.strikethrough) {
        sep();
        if (to.strikethrough) *p++ = '9';
        else                 { *p++ = '2'; *p++ = '9'; }
    }

    // Color::Default is the "occlude but emit no color SGR" sentinel —
    // skip it as if absent. fg / bg are emitted only when the
    // effective-color state actually changes.
    auto effective_fg = [](const std::optional<Color>& c) -> const Color* {
        if (c.has_value() && c->kind() != Color::Kind::Default) return &*c;
        return nullptr;
    };
    const Color* from_fg = effective_fg(from.fg);
    const Color* to_fg   = effective_fg(to.fg);
    if ((from_fg == nullptr) != (to_fg == nullptr)
        || (from_fg && to_fg && !(*from_fg == *to_fg)))
    {
        sep();
        if (to_fg) {
            p = append_color_sgr(p, *to_fg, /*is_fg=*/true);
        } else {
            // Explicit fg disabled → ANSI 39 (default fg).
            *p++ = '3'; *p++ = '9';
        }
    }

    const Color* from_bg = effective_fg(from.bg);
    const Color* to_bg   = effective_fg(to.bg);
    if ((from_bg == nullptr) != (to_bg == nullptr)
        || (from_bg && to_bg && !(*from_bg == *to_bg)))
    {
        sep();
        if (to_bg) {
            p = append_color_sgr(p, *to_bg, /*is_fg=*/false);
        } else {
            *p++ = '4'; *p++ = '9';
        }
    }

    // No SGR-affecting changes (can happen when two distinct style IDs
    // differ only in `dim`, which we don't emit). Skip emission entirely
    // — emitting `\x1b[m` would mean SGR reset, the opposite of "no-op".
    if (first) return;

    *p++ = 'm';
    out.append(buf, static_cast<std::size_t>(p - buf));
}

std::string StylePool::build_sgr(const Style& s) {
    char buf[64];
    char* p = buf;
    *p++ = '\x1b'; *p++ = '['; *p++ = '0';

    if (s.bold)          { *p++ = ';'; *p++ = '1'; }
    // SGR 2 (faint) deliberately suppressed. Windows Terminal renders
    // SGR 2 by reducing brightness ~50%, which on already-dark colors
    // (bright_black, dim cyan, etc.) collapses below the readable
    // contrast floor — entire chrome elements vanish on the user's
    // theme. Other terminals (Alacritty, kitty, iTerm2, Ghostty)
    // honor SGR 2 with milder reductions but still measurably hurt
    // contrast on low-contrast themes (Solarized, Gruvbox-light). The
    // hierarchy effect "dim" was meant to provide is already carried
    // by color choice (bright_black for muted text); dropping the SGR
    // attribute keeps the same color and stops the disappearing-text
    // bug. `s.dim` is still honored in hashing/equality so the cache
    // doesn't alias dim and non-dim styles, but it never reaches the
    // wire.
    // if (s.dim)           { *p++ = ';'; *p++ = '2'; }
    if (s.italic)        { *p++ = ';'; *p++ = '3'; }
    if (s.underline)     { *p++ = ';'; *p++ = '4'; }
    if (s.inverse)       { *p++ = ';'; *p++ = '7'; }
    if (s.strikethrough) { *p++ = ';'; *p++ = '9'; }

    // Color::Default is a sentinel meaning "occlude this cell but emit no
    // explicit color SGR" — preserves terminal background transparency
    // (e.g. Ghostty) while still letting the cell overdraw underlying glyphs.
    if (s.fg.has_value() && s.fg->kind() != Color::Kind::Default) {
        *p++ = ';'; p = append_color_sgr(p, *s.fg, true);
    }
    if (s.bg.has_value() && s.bg->kind() != Color::Kind::Default) {
        *p++ = ';'; p = append_color_sgr(p, *s.bg, false);
    }

    *p++ = 'm';
    return {buf, static_cast<std::size_t>(p - buf)};
}

std::size_t StylePool::hash_style(const Style& s) noexcept {
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

void StylePool::insert_slot(std::size_t h, uint16_t id) {
    std::size_t idx = h & mask_;
    while (slots_[idx].hash != 0) {
        idx = (idx + 1) & mask_;
    }
    slots_[idx] = {h, id};
}

void StylePool::grow(std::size_t new_cap) {
    capacity_ = new_cap;
    mask_ = new_cap - 1;
    slots_.assign(new_cap, Slot{});
    // Re-insert all existing styles.
    for (uint16_t i = 0; i < static_cast<uint16_t>(styles_.size()); ++i) {
        insert_slot(hash_style(styles_[i]), i);
    }
}

// ============================================================================
// Canvas
// ============================================================================

Canvas::Canvas(int width, int height, StylePool* pool)
    : width_(width)
    , height_(height)
    , style_pool_(pool)
    , damage_{{Columns{0}, Rows{0}}, {Columns{width}, Rows{height}}}
{
    cells_.resize(static_cast<std::size_t>(width_ * height_), default_cell());
    last_col_.assign(static_cast<std::size_t>(height_), -1);
}

Cell Canvas::get(int x, int y) const noexcept {
    if (!in_bounds(x, y)) return Cell{};
    return Cell::unpack(cells_[cell_index(x, y)]);
}

void Canvas::write_text(int x, int y, std::string_view text, uint16_t style_id) {
    if (__builtin_expect(!in_bounds(x, y), 0)) return;

    int cx = x;
    std::size_t pos = 0;
    const std::size_t len = text.size();
    const char* data = text.data();

    // ASCII fast path: most TUI text is ASCII. Batch-set without decode_utf8.
    while (pos < len) {
        // Scan run of printable ASCII bytes.
        while (pos < len) {
            auto byte = static_cast<unsigned char>(data[pos]);
            if (byte >= 0x80 || byte < 0x20) break;
            set(cx, y, static_cast<char32_t>(byte), style_id, 0);
            ++cx;
            ++pos;
        }

        // Handle non-ASCII / control characters.
        if (pos < len) {
            auto byte = static_cast<unsigned char>(data[pos]);
            if (byte < 0x20) { ++pos; continue; } // skip control
            char32_t cp = decode_utf8(text, pos);
            if (cp < 0x20) continue;
            if (is_wide_char(cp)) {
                set(cx, y, cp, style_id, 1);
                set(cx + 1, y, U' ', style_id, 2);
                cx += 2;
            } else {
                set(cx, y, cp, style_id, 0);
                ++cx;
            }
        }
    }
}

void Canvas::fill(Rect region, char32_t ch, uint16_t style_id) {
    int x0 = std::max(0, region.left().value);
    int y0 = std::max(0, region.top().value);
    int x1 = std::min(width_, region.right().value);
    int y1 = std::min(height_, region.bottom().value);

    // Apply active clip in one shot — no per-pixel check needed.
    if (has_clip_) {
        x0 = std::max(x0, clip_x0_);
        y0 = std::max(y0, clip_y0_);
        x1 = std::min(x1, clip_x1_);
        y1 = std::min(y1, clip_y1_);
    }

    if (x0 >= x1 || y0 >= y1) return;

    uint64_t packed = Cell{ch, style_id, 0, 0}.pack();
    uint64_t* base  = cells_.data();

    // For full-width fills spanning many rows, use SIMD streaming fill to
    // avoid polluting L1/L2 cache (the data will be read later by diff).
    // Threshold: 4+ full rows ≈ 320+ cells at width=80.
    const bool full_width = (x0 == 0 && x1 == width_);
    if (full_width && (y1 - y0) >= 4) {
        simd::streaming_fill(base + y0 * width_, static_cast<std::size_t>((y1 - y0) * width_), packed);
    } else {
        for (int y = y0; y < y1; ++y) {
            std::fill(base + y * width_ + x0, base + y * width_ + x1, packed);
        }
    }

    // Track max painted row + per-row last-content column for non-blank fills.
    if (ch != U' ' || style_id != 0) {
        if (y1 - 1 > max_y_) max_y_ = y1 - 1;
        const int new_last = x1 - 1;
        for (int y = y0; y < y1; ++y) {
            if (new_last > last_col_[static_cast<std::size_t>(y)])
                last_col_[static_cast<std::size_t>(y)] = new_last;
        }
    }
    stage_ = CanvasStage::Painted;
}

void Canvas::clear() {
    uint64_t blank = default_cell();
    // Bounded clear: only fill rows that had visible content last frame.
    //
    // Invariant: cells beyond max_y_ are always blank. Holds inductively:
    //   - Construction (cells_.resize(..., default_cell())) initialises
    //     every cell to blank, max_y_ starts at -1.
    //   - resize(w, h) re-assigns the whole grid to blank, resets max_y_.
    //   - Every prior clear() cleared up to its own max_y_+1. Rows it
    //     left untouched were either never painted (still blank from
    //     construction) or were blanked by an earlier clear (because
    //     that earlier max_y_ was >= those rows).
    //
    // So we can fill only [0, max_y_+1) and rows beyond are guaranteed
    // blank already. For inline mode where a 500-row canvas typically
    // holds ~50 rows of content, this is ~10× less memory traffic per
    // frame (the streaming_fill is the dominant clear cost).
    //
    // damage_ stays full_rect: paint may extend past the previous
    // max_y_ this frame, and the diff has to cover that new region.
    const int rows_to_clear = std::clamp(max_y_ + 1, 0, height_);
    if (rows_to_clear > 0) {
        simd::streaming_fill(cells_.data(),
                             static_cast<std::size_t>(rows_to_clear * width_),
                             blank);
        std::fill(last_col_.begin(), last_col_.begin() + rows_to_clear, -1);
    }
    damage_ = full_rect();
    max_y_ = -1;
    stage_ = CanvasStage::Drained;
}

void Canvas::clear_rows(int n) {
    if (n <= 0) return;
    int rows = std::min(n, height_);
    auto count = static_cast<std::size_t>(rows * width_);
    uint64_t blank = default_cell();
    // For small regions (typical inline mode), regular fill keeps L1 warm.
    std::fill(cells_.data(), cells_.data() + count, blank);
    damage_ = Rect{{Columns{0}, Rows{0}}, {Columns{width_}, Rows{rows}}};
    std::fill(last_col_.begin(), last_col_.begin() + rows, -1);
    // Partial clear: rows [n, height_) may still hold non-blank content,
    // so max_y_ is NOT canvas-wide -1. Rescan the surviving rows from
    // the bottom up via last_col_ (kept current by set/blit/fill).
    // The previous unconditional `max_y_ = -1` left a corrupt state
    // where last_content_col(y) for y >= n returned a valid column
    // but max_content_row() returned -1, so content_height() said
    // "0 rows" and the renderer would skip emitting the surviving
    // rows entirely — silent corruption (live content invisible to
    // the diff scan, but still on the wire from the prior frame).
    int new_max_y = -1;
    for (int y = height_ - 1; y >= rows; --y) {
        if (last_col_[static_cast<std::size_t>(y)] >= 0) {
            new_max_y = y;
            break;
        }
    }
    max_y_ = new_max_y;
    stage_ = CanvasStage::Drained;
}

void Canvas::push_clip(Rect clip) {
    if (clip_stack_.empty()) {
        clip_stack_.push_back(clip);
    } else {
        // Intersect with the current effective clip.
        clip_stack_.push_back(clip_stack_.back().intersect(clip));
    }
    update_clip_cache();
}

void Canvas::pop_clip() {
    if (!clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
    update_clip_cache();
}

void Canvas::reset_clips() noexcept {
    clip_stack_.clear();
    update_clip_cache();
}

bool Canvas::is_clipped(int x, int y) const noexcept {
    if (!has_clip_) return false;
    return x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_;
}

void Canvas::resize(int w, int h) {
    width_ = w;
    height_ = h;
    max_y_ = -1;
    cells_.assign(static_cast<std::size_t>(w * h), default_cell());
    last_col_.assign(static_cast<std::size_t>(h), -1);
    damage_ = full_rect();
    clip_stack_.clear();
    update_clip_cache();
    stage_ = CanvasStage::Drained;
}

void Canvas::mark_damage(Rect region) {
    damage_ = damage_.unite(region);
}

void Canvas::mark_all_damaged() {
    damage_ = full_rect();
}

} // namespace maya
