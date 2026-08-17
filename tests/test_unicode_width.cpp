// Golden-value tests for the generated Unicode width table.
//
// These cases exercise:
//   - ASCII / Latin-1 (always 1 column)
//   - East Asian Wide / Fullwidth (always 2 columns, matches every terminal)
//   - Emoji_Presentation (2 columns under WidthMode::Modern, 1 under Legacy)
//   - C0 control codes (always 0)
//   - Boundary code points around the most complex range — Misc Symbols /
//     Dingbats — where the per-codepoint Emoji_Presentation property
//     creates non-trivial holes in the wide table.
//
// The values come from the official Unicode 16.0 UCD; bumping the table
// to a newer revision should not regress any of these.

#include "agtest.hpp"
#include <print>

#include <maya/text/unicode_width.hpp>

namespace {

constexpr int W(char32_t cp,
                maya::unicode::WidthMode m = maya::unicode::WidthMode::Modern) {
    return maya::unicode::char_width(cp, m);
}

TEST_CASE("ascii and latin") {
    std::println("--- test_ascii_and_latin ---");
    static_assert(W('A')      == 1);
    static_assert(W('z')      == 1);
    static_assert(W('0')      == 1);
    static_assert(W(' ')      == 1);
    static_assert(W('.')      == 1);  // the dot the user kept losing
    static_assert(W(0x00E9)   == 1);  // é
    static_assert(W(0x03B1)   == 1);  // α (Greek alpha — narrow)
    std::println("PASS");
}

TEST_CASE("control codes") {
    std::println("--- test_control_codes ---");
    static_assert(W(0x00) == 0);
    static_assert(W(0x09) == 0);  // tab
    static_assert(W(0x1F) == 0);  // last C0
    static_assert(W(0x20) == 1);  // first non-control: space
    std::println("PASS");
}

TEST_CASE("is control") {
    std::println("--- test_is_control ---");
    using maya::unicode::is_control;
    // is_control() is the SECURITY predicate: a renderer drops these before
    // emitting text so untrusted input (process names, filenames, pasted
    // blobs) can't inject a terminal escape sequence. It must catch C0, DEL,
    // AND the C1 block — the last is the part naive `< 0x20` filters miss,
    // and it contains the 8-bit CSI/OSC/DCS/ST introducers.
    static_assert(is_control(0x00));
    static_assert(is_control(0x1B));  // ESC
    static_assert(is_control(0x07));  // BEL
    static_assert(is_control(0x0A));  // newline (a control; callers keep it explicitly)
    static_assert(is_control(0x1F));  // last C0
    static_assert(is_control(0x7F));  // DEL
    static_assert(is_control(0x80));  // first C1
    static_assert(is_control(0x90));  // DCS
    static_assert(is_control(0x9B));  // CSI (8-bit)
    static_assert(is_control(0x9C));  // ST
    static_assert(is_control(0x9D));  // OSC
    static_assert(is_control(0x9F));  // last C1
    // Printable / legitimate text must NOT be flagged — no false positives
    // that would mangle real names. U+00A0 (nbsp) sits just past C1.
    static_assert(!is_control(0x20));    // space
    static_assert(!is_control('A'));
    static_assert(!is_control(0x7E));    // ~
    static_assert(!is_control(0xA0));    // no-break space (NOT a control)
    static_assert(!is_control(0x00E9));  // é
    static_assert(!is_control(0x4E2D));  // 中
    static_assert(!is_control(0x1F680)); // 🚀
    std::println("PASS");
}

TEST_CASE("combining marks") {
    std::println("--- test_combining_marks ---");
    // Combining marks stack on the preceding base glyph and add NO columns.
    // Counting them as 1 over-measures every accented string and shoves
    // following content (and any border) one cell right — the bug that made
    // \vec{v} / \hat{x} leak past a math block's border.
    static_assert(W(0x0300) == 0);  // combining grave
    static_assert(W(0x0301) == 0);  // combining acute (é = e + this)
    static_assert(W(0x0302) == 0);  // circumflex (x̂)
    static_assert(W(0x0304) == 0);  // macron / overline (\bar)
    static_assert(W(0x0307) == 0);  // dot above (\dot)
    static_assert(W(0x030C) == 0);  // caron (\check)
    static_assert(W(0x036F) == 0);  // last Combining Diacritical Marks
    static_assert(W(0x20D7) == 0);  // combining right arrow above (\vec)
    static_assert(W(0x20D0) == 0);  // first Combining Marks for Symbols
    static_assert(W(0x20FF) == 0);  // last of that block
    static_assert(W(0x1DC0) == 0);  // Combining Diacritical Marks Supplement
    static_assert(W(0xFE20) == 0);  // Combining Half Marks
    static_assert(W(0x200B) == 0);  // zero-width space
    static_assert(W(0x200D) == 0);  // zero-width joiner
    static_assert(W(0xFEFF) == 0);  // BOM / zero-width no-break space
    // The base characters around them stay width 1.
    static_assert(W(0x02FF) == 1);  // just before the combining block
    static_assert(W(0x0370) == 1);  // just after (Greek)
    std::println("PASS");
}

TEST_CASE("east asian wide") {
    std::println("--- test_east_asian_wide ---");
    // CJK Unified — every terminal in existence renders these as 2 cols.
    static_assert(W(U'中')    == 2);  // U+4E2D
    static_assert(W(U'文')    == 2);  // U+6587
    static_assert(W(0x4E00)   == 2);  // first CJK
    static_assert(W(0x9FFF)   == 2);
    static_assert(W(0x1100)   == 2);  // Hangul Jamo
    static_assert(W(0xAC00)   == 2);  // Hangul syllable 가
    static_assert(W(0x3000)   == 2);  // Ideographic space
    static_assert(W(0xFF21)   == 2);  // Fullwidth Latin A

    // Legacy mode also returns 2 — these are W/F in EAW, not just emoji.
    static_assert(W(U'中', maya::unicode::WidthMode::Legacy) == 2);
    std::println("PASS");
}

TEST_CASE("emoji presentation") {
    std::println("--- test_emoji_presentation ---");
    // The bug-of-record. ⚡ is in Misc Symbols (U+2600 block) but
    // Unicode 16.0 EAW classifies it as Wide, so it's 2 cols on Windows
    // Terminal / Kitty / iTerm 3.5+ / WezTerm / Alacritty / Ghostty /
    // vte 0.62+. (Older terminals that ignored the EAW=W classification
    // are simply non-conformant — the mismatch was never our bug.)
    static_assert(W(0x26A1)   == 2);  // ⚡
    static_assert(W(0x2705)   == 2);  // ✅
    static_assert(W(0x274C)   == 2);  // ❌
    static_assert(W(0x2728)   == 2);  // ✨
    static_assert(W(0x231A)   == 2);  // ⌚
    static_assert(W(0x1F600)  == 2);  // 😀
    static_assert(W(0x1F680)  == 2);  // 🚀
    static_assert(W(0x1F9E0)  == 2);  // 🧠

    // Codepoints in the same blocks WITHOUT Emoji_Presentation AND
    // without EAW=W stay narrow.
    static_assert(W(0x2600)   == 1);  // ☀ — Emoji but not Emoji_Presentation
    static_assert(W(0x2603)   == 1);  // ☃ snowman — same story
    static_assert(W(0x26A0)   == 1);  // ⚠ — Emoji but not Emoji_Presentation

    // The Modern-vs-Legacy split: Regional Indicators (flag halves) are
    // Emoji_Presentation but Unicode classifies them EAW=Neutral. Modern
    // terminals render them as 2 cols (they pair into flag glyphs);
    // Legacy renders as 1.
    static_assert(W(0x1F1E6, maya::unicode::WidthMode::Modern) == 2);
    static_assert(W(0x1F1E6, maya::unicode::WidthMode::Legacy) == 1);
    std::println("PASS");
}

TEST_CASE("no off by one") {
    std::println("--- test_no_off_by_one ---");
    // Boundary checks on the most common wide ranges.
    static_assert(W(0x10FF) == 1);  // just before Hangul Jamo
    static_assert(W(0x1100) == 2);  // first Hangul Jamo
    static_assert(W(0x115F) == 2);  // last Hangul Jamo
    static_assert(W(0x1160) == 1);  // just after — Hangul Jamo medial vowels (narrow)

    // Yijing Hexagrams (U+4DC0..4DFF) are EAW=W per Unicode 16.0.
    static_assert(W(0x4DBF) == 2);  // last CJK Ext A
    static_assert(W(0x4DC0) == 2);  // first Yijing
    static_assert(W(0x4DFF) == 2);  // last Yijing
    static_assert(W(0x4E00) == 2);  // first CJK Unified
    static_assert(W(0x9FFF) == 2);  // last CJK Unified

    // Single-codepoint Emoji_Presentation island around ⚡.
    static_assert(W(0x26A0) == 1);  // ⚠ — Emoji but not Emoji_Presentation
    static_assert(W(0x26A1) == 2);  // ⚡ — the one
    static_assert(W(0x26A2) == 1);  // just past ⚡ — narrow

    std::println("PASS");
}

} // namespace

