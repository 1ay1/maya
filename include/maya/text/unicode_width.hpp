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
#include <string_view>

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
///   0 — control chars (<0x20), and zero-width combining marks / joiners
///       (they stack on the preceding base glyph and add no columns)
///   1 — narrow / neutral / ambiguous
///   2 — East_Asian_Wide / Fullwidth, plus Emoji_Presentation under WidthMode::Modern
///
/// `consteval` callers (compile-time string-width computation in widget
/// builders) get a fully-folded constant. Runtime callers pay one O(log n)
/// binary search per code point — fast enough to call inside the hot
/// per-cell text-shaping loop.
[[nodiscard]] constexpr bool is_zero_width(char32_t cp) noexcept {
    // Combining marks and format controls occupy no display columns — they
    // compose onto the preceding base character (é = e + U+0301, x̂, v⃗). A
    // renderer that counts them as 1 column over-measures every accented
    // string and pushes following content one cell to the right. These are
    // the Unicode combining-mark blocks plus the common zero-width formatters.
    return (cp >= 0x0300 && cp <= 0x036F) ||   // Combining Diacritical Marks
           (cp >= 0x0483 && cp <= 0x0489) ||   // Cyrillic combining
           (cp >= 0x0591 && cp <= 0x05BD) ||   // Hebrew points (subset)
           (cp >= 0x0610 && cp <= 0x061A) ||   // Arabic marks (subset)
           (cp >= 0x064B && cp <= 0x065F) ||   // Arabic diacritics
           cp == 0x0670                    ||   // Arabic superscript alef
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||   // Combining Diacritical Ext.
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||   // Combining Diacritical Supp.
           (cp >= 0x20D0 && cp <= 0x20FF) ||   // Combining Marks for Symbols
           (cp >= 0xFE20 && cp <= 0xFE2F) ||   // Combining Half Marks
           cp == 0x200B || cp == 0x200C ||     // ZWSP, ZWNJ
           cp == 0x200D || cp == 0xFEFF;       // ZWJ, ZWNBSP/BOM
}

[[nodiscard]] constexpr bool is_control(char32_t cp) noexcept {
    // Control codepoints that must NEVER be emitted verbatim to the terminal:
    // they ARE, or INTRODUCE, in-band escape sequences the emulator acts on.
    // Rendering an untrusted one is a terminal-escape-injection vector — a
    // hostile process name, filename, branch name or pasted blob could move
    // the cursor, rewrite scrollback, or via OSC tamper with the window title
    // or clipboard. A renderer that draws externally-sourced text must drop
    // these; callers should route untrusted strings through this predicate.
    //   C0  U+0000..U+001F  — incl. ESC 0x1B, BEL, CR, BS, TAB
    //   DEL U+007F
    //   C1  U+0080..U+009F  — the 8-bit introducers CSI (0x9B), OSC (0x9D),
    //                         DCS (0x90), ST (0x9C). A UTF-8-encoded C1 (e.g.
    //                         0xC2 0x9B) decodes to this range, so a single
    //                         post-decode codepoint check catches both the
    //                         raw-byte and UTF-8 forms.
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

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
    //
    // EXCEPTION: combining diacritics live at U+0300+ (below U+1100) and are
    // zero-width. Check them before the short-circuit so accented text and
    // math accents (\hat \bar \vec) measure correctly.
    if (cp >= 0x0300 && is_zero_width(cp)) return 0;
    if (cp < 0x1100) return 1;
    // SECOND fast path: box-drawing (U+2500..257F) + block elements
    // (U+2580..259F) are the glyphs that fill every bordered TUI — table
    // frames, code fences, the changes strip, progress bars. They are ALL
    // width-1 and this whole 0x2500..0x259F block is verified to contain no
    // Wide/Fullwidth or zero-width codepoint (the nearest wide neighbour is
    // U+25FD). Short-circuiting here skips BOTH O(log n) binary searches for
    // a class of glyph that recurs on essentially every rendered border row,
    // re-measured every layout pass during the streaming glide — profiled as
    // the dominant width-lookup cost (box glyphs were ~5.7x an ASCII char
    // before this). (Keep the upper bound < U+25FD, the first wide entry.)
    if (cp >= 0x2500 && cp <= 0x259F) return 1;
    if (is_zero_width(cp)) return 0;             // U+1AB0+, U+20D0+, U+FE20+ …
    if (detail::in_ranges(cp, detail::kWideRanges)) return 2;
    if (mode == WidthMode::Modern &&
        detail::in_ranges(cp, detail::kEmojiPresentationRanges)) return 2;
    return 1;
}

/// Display width of a UTF-8 string, in terminal columns.
///
/// `consteval`-usable, and THAT is the point: a widget that lays out around a
/// literal (a separator, a chip's padding, an icon) can compute its width
/// FROM the literal instead of restating it as an integer beside it.
///
/// The hand-copied form is a drift bomb. Real instance: status_bar.hpp held
///
///     constexpr int kSepW = 7;   // "   ·   "
///
/// and narrowing the separator to " · " left the constant reserving four
/// columns that were no longer painted — so the title chip over-shed on
/// exactly the narrow terminals the shed ladder exists for. Nothing in the
/// type system connected the two; a human had to remember.
///
///     static constexpr auto kSep  = " \xc2\xb7 ";
///     static constexpr int  kSepW = str_width(kSep);   // cannot drift
///
/// Invalid UTF-8 bytes count as one column each — total, never throws, so a
/// malformed literal degrades to a plausible width rather than a crash.
[[nodiscard]] constexpr int str_width(
    std::string_view s,
    WidthMode mode = WidthMode::Modern) noexcept
{
    int w = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        char32_t cp  = 0;
        std::size_t n = 0;
        if      (b0 < 0x80)          { cp = b0;        n = 1; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; n = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; n = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; n = 4; }
        else                          { ++w; ++i; continue; }   // stray byte

        if (i + n > s.size()) { ++w; ++i; continue; }            // truncated
        bool ok = true;
        for (std::size_t k = 1; k < n; ++k) {
            const auto bk = static_cast<unsigned char>(s[i + k]);
            if ((bk & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (bk & 0x3F);
        }
        if (!ok) { ++w; ++i; continue; }

        w += char_width(cp, mode);
        i += n;
    }
    return w;
}

} // namespace maya::unicode
