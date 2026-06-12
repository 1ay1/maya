#pragma once
// maya::text::char_width — Unicode display-width lookup, constexpr-ready.
//
// Two layers of decision:
//
//   1. East_Asian_Width = Wide / Fullwidth (kWideRanges).
//      CJK ideographs, Hangul syllables, fullwidth Latin, the East-Asian-
//      bracket pair, AND most emoji-presentation code points (Unicode
//      9+ promoted them to W in EAW). Used unconditionally — terminals
//      that render these narrow are simply non-conformant with the
//      Unicode standard.
//
//   2. Emoji_Presentation (kEmojiPresentationRanges).
//      The small handful of code points that have Emoji_Presentation
//      = Yes but Neutral EAW — chiefly the Regional Indicators
//      U+1F1E6–U+1F1FF (flag-half components). Modern terminals
//      pair these into flag glyphs and render each half as 2 cols;
//      legacy terminals render them as single narrow boxes.
//
// `WidthMode::Modern` consults both tables; `WidthMode::Legacy` consults
// only the EAW one. is_wide_char() in element/text.hpp defaults to Modern.
//
// Both tables are *generated* from the official Unicode UCD and pinned
// in maya/data/. Regenerate via:
//
//     python maya/scripts/gen_unicode_width.py
//
// Lookup is a constexpr binary search over the sorted, coalesced range
// arrays. Compilers fold widths of literal strings at compile time —
// the per-character cost at runtime is ~9 comparisons.

#include <algorithm>
#include <cstdint>
#include <span>

#include "unicode_width_table.hpp"

namespace maya::unicode {

enum class WidthMode : std::uint8_t {
    /// East_Asian_Width only. Use on legacy terminals (or in tests where
    /// you want behaviour that matches every emulator ever built).
    Legacy,
    /// East_Asian_Width + Emoji_Presentation. The default — matches the
    /// vast majority of terminals shipping in 2026.
    Modern,
};

namespace detail {

/// Constexpr binary search over a sorted, non-overlapping range table.
/// Returns true iff `cp` falls within any [first, last] entry.
[[nodiscard]] constexpr bool in_ranges(
    char32_t cp,
    std::span<const WidthRange> ranges) noexcept
{
    // upper_bound by `first` finds the first range whose start is past
    // `cp`; the candidate (if any) is its predecessor — we then check
    // whether `cp` is inside that range's [first, last] span.
    auto it = std::ranges::upper_bound(
        ranges, cp, {}, &WidthRange::first);
    if (it == ranges.begin()) return false;
    --it;
    return cp <= it->last;
}

} // namespace detail

/// Display width of a single Unicode code point, in terminal columns.
///
///   0 — control chars (<0x20, except a few combining marks not modelled here)
///   1 — narrow / neutral / ambiguous
///   2 — East_Asian_Wide / Fullwidth, plus Emoji_Presentation under WidthMode::Modern
///
/// `consteval` callers (compile-time string-width computation in widget
/// builders) get a fully-folded constant. Runtime callers pay one O(log n)
/// binary search per code point — fast enough to call inside the hot
/// per-cell text-shaping loop.
[[nodiscard]] constexpr int char_width(
    char32_t cp,
    WidthMode mode = WidthMode::Modern) noexcept
{
    if (cp < 0x20) return 0;                     // C0 control codes
    // Everything below U+1100 — the first entry in kWideRanges — is a single
    // column. This covers ASCII, Latin-1, the box-drawing (U+2500..) and
    // block-element (U+2580..) glyphs that fill every bordered TUI, plus
    // Greek/Cyrillic/etc. Short-circuit so the per-cell text-shaping hot
    // path skips BOTH O(log n) binary searches for the overwhelmingly
    // common case. (Keep this bound in sync with kWideRanges[0].first.)
    if (cp < 0x1100) return 1;
    if (detail::in_ranges(cp, detail::kWideRanges)) return 2;
    if (mode == WidthMode::Modern &&
        detail::in_ranges(cp, detail::kEmojiPresentationRanges)) return 2;
    return 1;
}

} // namespace maya::unicode
