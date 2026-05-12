#pragma once
// maya::element::text - Text element: a leaf node that displays styled text
//
// Handles word wrapping, truncation, and Unicode-aware width computation.
// TextElement satisfies the Measurable concept: given a max_width constraint,
// it returns the Size it would occupy.

#include "../core/types.hpp"
#include "../style/style.hpp"
#include "../text/unicode_width.hpp"

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
// Width detection is delegated to maya::text::char_width(), which binary-
// searches a constexpr range table generated from the official Unicode
// UCD (East_Asian_Width.txt + emoji-data.txt, version 16.0, pinned in
// maya/data/). To bump the Unicode revision, drop newer .txt files into
// maya/data/ and re-run `python maya/scripts/gen_unicode_width.py`.
//
// `is_wide_char` keeps its old name and signature for source-compat; it
// asks for WidthMode::Modern (matches Windows Terminal, Kitty, iTerm 3.5+,
// WezTerm, Alacritty, Ghostty, vte ≥ 0.62 — i.e. every terminal shipping
// today). Callers that need the legacy 1990s wcwidth interpretation can
// call maya::text::char_width(cp, WidthMode::Legacy) directly.

/// Returns true iff the code point occupies two terminal columns.
[[nodiscard]] constexpr bool is_wide_char(char32_t cp) noexcept {
    return unicode::char_width(cp, unicode::WidthMode::Modern) == 2;
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
// These respect UTF-8 codepoint boundaries AND display widths — wide
// (CJK / emoji) characters count as 2 columns, multi-byte sequences are
// never split mid-codepoint.  Widgets that need to fit text into a
// column budget MUST use these instead of `s.substr(0, max_w-1) + "…"`,
// which silently corrupts multi-byte content (split UTF-8 → invalid
// renders, byte-size off by N for wide chars → padding misalignment).

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

// ── Public re-exports — promote the safe truncation helpers out of
// `detail::` so widgets can find them without leaning on internal
// namespaces.  Widget authors should ALWAYS use these instead of
// `s.substr(0, n)` (byte-indexed, splits codepoints) or
// `if (s.size() > N) s.resize(N)` (byte size != display width).
using detail::truncate_end;
using detail::truncate_start;
using detail::truncate_middle;

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

/// One display-ready line produced by TextElement::format().
///
/// `byte_offset` is the position within the source `content` where this
/// line's text begins. For TextWrap::Wrap / NoWrap this is the natural
/// offset of the wrapped substring (which the painter uses to align
/// styled runs to wrapped output without a per-frame O(content × lines)
/// substring search).
///
/// For truncation modes (TruncateEnd / TruncateStart / TruncateMiddle)
/// `text` is synthetic — it contains an inserted ellipsis that isn't
/// present in `content`, so `byte_offset` is left as 0 and the painter
/// uses a separate explicit-mapping branch for those modes.
struct WrappedLine {
    std::string text;
    std::size_t byte_offset = 0;
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
    mutable std::vector<WrappedLine> cached_lines;

    // -- Measurement ---------------------------------------------------------

    /// Compute the Size this text would occupy given a maximum width constraint.
    /// Handles word wrapping, truncation, and Unicode-aware width.
    [[nodiscard]] Size measure(int max_width) const;

    // -- Rendering helpers ---------------------------------------------------

    /// Return the display-ready lines for a given width constraint.
    /// Word-wrapped or truncated as configured. Each WrappedLine carries
    /// both the text and the byte offset into `content` where it
    /// originated (used by the renderer to align styled runs without an
    /// O(content × lines) substring search). The reference is valid as
    /// long as the element lives.
    [[nodiscard]] const std::vector<WrappedLine>& format(int max_width) const;
};

} // namespace maya
