#pragma once
// maya::element::text - Text element: a leaf node that displays styled text
//
// Handles word wrapping, truncation, and Unicode-aware width computation.
// TextElement satisfies the Measurable concept: given a max_width constraint,
// it returns the Size it would occupy.

#include "../core/types.hpp"
#include "../style/style.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

// ============================================================================
// TextWrap - How text reflows when it exceeds available width
// ============================================================================

enum class TextWrap : uint8_t {
    Wrap,             ///< Break at word boundaries (default).
    TruncateEnd,      ///< Cut from the right, append ellipsis.
    TruncateMiddle,   ///< Keep start and end, ellipsis in center.
    TruncateStart,    ///< Cut from the left, prepend ellipsis.
    NoWrap,           ///< Never break; may overflow the container.
};

// ============================================================================
// Unicode width utilities
// ============================================================================
// A simplified CJK / fullwidth character detector. Covers the most common
// ranges used in terminal rendering. East Asian Wide (W) and Fullwidth (F)
// code points occupy two terminal columns; everything else occupies one.

/// Returns true if the Unicode code point is a wide character (CJK, etc.)
/// that occupies two terminal columns.
/// Ranges match the East Asian Width property (W=Wide, F=Fullwidth) from
/// Unicode, aligned with the reference implementation in Claude Code ($N1).
[[nodiscard]] constexpr bool is_wide_char(char32_t cp) noexcept {
    // Hangul Jamo (leading consonants) — wide in most terminals
    if (cp >= 0x1100  && cp <= 0x115F)  return true;
    // CJK Radicals Supplement, Kangxi Radicals, Ideographic Description,
    // CJK Symbols & Punct, Hiragana, Katakana, Bopomofo, Hangul Compat Jamo,
    // Kanbun, Bopomofo Extended, CJK Strokes (U+2E80-U+3247)
    if (cp >= 0x2E80  && cp <= 0x3247)  return true;
    // CJK angle brackets
    if (cp == 0x2329 || cp == 0x232A)   return true;
    // CJK Compat, Extension A, Yijing (U+3250-U+4DBF)
    if (cp >= 0x3250  && cp <= 0x4DBF)  return true;
    // CJK Unified Ideographs, Yi Syllables/Radicals (U+4E00-U+A4C6)
    if (cp >= 0x4E00  && cp <= 0xA4C6)  return true;
    // Hangul Syllables
    if (cp >= 0xAC00  && cp <= 0xD7AF)  return true;
    // CJK Compatibility Ideographs
    if (cp >= 0xF900  && cp <= 0xFAFF)  return true;
    // Fullwidth Latin, Greek, Cyrillic, etc.
    if (cp >= 0xFF01  && cp <= 0xFF60)  return true;
    // Fullwidth currency and other signs
    if (cp >= 0xFFE0  && cp <= 0xFFE6)  return true;
    // Enclosed Ideographic Supplement
    if (cp >= 0x1F200 && cp <= 0x1F251) return true;
    // Emoji: Miscellaneous Symbols and Pictographs, Emoticons, Ornamental
    // Dingbats, Transport/Map Symbols, Supplemental Symbols, Playing Cards.
    // These are always rendered as 2 cells in modern terminals.
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return true;
    // Supplemental Symbols and Pictographs
    if (cp >= 0x1FA00 && cp <= 0x1FAFF) return true;
    // CJK Unified Ideographs Extension B through end of TIP (U+20000-U+3FFFD)
    if (cp >= 0x20000 && cp <= 0x3FFFD) return true;
    return false;
}

/// Decode the next UTF-8 code point from a string_view.
/// Advances `pos` past the decoded bytes. Returns U+FFFD on invalid input.
[[nodiscard]] char32_t decode_utf8(std::string_view sv, std::size_t& pos) noexcept;

/// Compute the display width of a UTF-8 string in terminal columns.
/// Wide (CJK) characters count as 2; all others count as 1.
/// Control characters (< 0x20) and zero-width code points are ignored.
[[nodiscard]] int string_width(std::string_view text) noexcept;

// ============================================================================
// word_wrap - Split text into lines that fit within a column budget
// ============================================================================
// Breaks at whitespace boundaries. If a single word exceeds max_width, it
// is force-broken at the column limit. Returns a vector of string_views
// pointing into the original text.

[[nodiscard]] std::vector<std::string_view>
word_wrap(std::string_view text, int max_width);

// ============================================================================
// Truncation helpers
// ============================================================================

namespace detail {

/// Truncate a string to fit within `max_width` columns, appending an ellipsis.
[[nodiscard]] std::string
truncate_end(std::string_view text, int max_width);

/// Truncate from the start, prepending an ellipsis.
[[nodiscard]] std::string
truncate_start(std::string_view text, int max_width);

/// Truncate from the middle, placing an ellipsis in the center.
[[nodiscard]] std::string
truncate_middle(std::string_view text, int max_width);

} // namespace detail

// ============================================================================
// TextElement - A leaf element that displays styled text
// ============================================================================
// Satisfies the Measurable concept. During layout, measure() computes
// the Size this text would occupy given a column constraint.

// ============================================================================
// StyledRun — a byte range within a TextElement with its own style
// ============================================================================
// Used for inline rich text (e.g. markdown paragraphs with bold/italic spans).
// The `content` field holds the full concatenated text for measurement and
// word wrapping.  Runs map byte ranges back to per-span styles for painting.

struct StyledRun {
    std::size_t byte_offset = 0;
    std::size_t byte_length = 0;
    Style       style;
};

struct TextElement {
    std::string content;
    Style       style = {};
    TextWrap    wrap = TextWrap::Wrap;

    /// Styled runs within content.  When non-empty, the renderer paints each
    /// run with its own style instead of using the base `style` for the whole
    /// element.  Runs must cover the entire content in order and not overlap.
    std::vector<StyledRun> runs = {};

    // -- Wrap cache ----------------------------------------------------------
    // Word wrap + truncation is O(content) per call; for finalized/cached
    // trees (e.g. a moha message's markdown Element reused across frames),
    // the same TextElement instance is measured and formatted every frame
    // at the same width. Cache by (width, wrap, content.size()) — callers
    // treat content + wrap as immutable once the element is handed to the
    // renderer, so no setters are needed for invalidation.
    mutable int                      cached_width = -1;
    mutable std::size_t              cached_content_size = 0;
    mutable TextWrap                 cached_wrap = TextWrap::Wrap;
    mutable Size                     cached_size{Columns{0}, Rows{1}};
    mutable std::vector<std::string> cached_lines;

    // -- Measurement ---------------------------------------------------------

    /// Compute the Size this text would occupy given a maximum width constraint.
    /// Handles word wrapping, truncation, and Unicode-aware width.
    [[nodiscard]] Size measure(int max_width) const;

    // -- Rendering helpers ---------------------------------------------------

    /// Return the display-ready lines for a given width constraint.
    /// Word-wrapped or truncated as configured.  Returned by const-ref
    /// against the element's cache; the reference is valid as long as the
    /// element lives.
    [[nodiscard]] const std::vector<std::string>& format(int max_width) const;
};

} // namespace maya
