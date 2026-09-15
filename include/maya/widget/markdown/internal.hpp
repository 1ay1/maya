// internal.hpp — shared declarations across the markdown TUs.
//
// The original monolithic widget/markdown.cpp was carved along its natural
// boundaries into parser.cpp / syntax.cpp / render.cpp / streaming.cpp.
// Symbols those TUs need to call across the boundary live here, in the
// internal `maya::md_detail` namespace. Private to the implementation —
// NOT installed and NOT included by public consumers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"

namespace maya {

// ============================================================================
// Terminal-adaptive color palette.
//
// Originally a file-scope `namespace colors` in markdown.cpp; lifted here
// so render.cpp (which builds full blocks) and streaming.cpp (which builds
// the in-flight tail) reference the same constants. All ANSI named-colors
// so the rendering follows the user's terminal theme (Catppuccin, Dracula,
// Solarized, etc.).
//
// DESIGN GOAL — visual hierarchy. The eye needs landmarks to scan a long
// reply. Earlier iteration mapped almost everything to bright_white /
// bright_black, which made bold body indistinguishable from h1 from kbd
// from definition-list terms. New mapping (terminal-theme adaptive — we
// only name the 16 ANSI slots, the user's theme picks the actual hex):
//
//   text          — `white` (the *normal* slot). Reads as paragraph body.
//                   Bold-bright contrast then actually means something.
//   bold_fg       — `bright_white` (the bright slot). Bold pops *above*
//                   body without color noise.
//   italic_fg     — left to inherit (no `with_fg`) so italic carries
//                   only the slant, not a competing color.
//   heading1      — `bright_cyan` + bold + rule. Strongest landmark.
//   heading2      — `cyan`        + bold + rule. One step lighter.
//   heading3      — `bright_blue` + bold. No rule (typographic weight
//                   alone is enough at this level).
//   heading_dim   — `blue`. h4–h6 collapse to a single muted blue —
//                   they're rare in agent output, no need to over-
//                   differentiate.
//   code_fg       — `cyan`. Yellow (the previous choice) screams; cyan
//                   reads as 'monospaced different' without dominating.
//   link_fg       — `bright_blue` + underline (underline kept in render).
//   quote_bar     — `bright_yellow`. A blockquote is a *callout*, not a
//                   comment; a colored gutter makes it land.
//   quote_text    — `white` (kept italic in render). Same weight as body
//                   so the gutter does the work.
//   list_bullet   — `bright_blue`. Matches link/heading family; the bullet
//                   is now a real glyph, not a smudge against background.
//   list_num      — `bright_blue` + bold. Ordered-list numerals deserve
//                   the same recognition as headings; they ARE the
//                   structure.
//   table_border  — `bright_black` (kept).
//   table_header  — `bright_cyan` + bold (matches h1). Header row reads
//                   as a heading-of-the-table.
//   alert_*       — kept on their semantic ANSI slot.
// ============================================================================
namespace colors {
    // Body prose uses the terminal's DEFAULT foreground rather than
    // an explicit ANSI 7. Two wins from this one change:
    //   1. The body adopts the user's carefully-tuned theme color
    //      (typically a soft warm/cool gray on dark themes, a near
    //      black on light), so prose feels native instead of
    //      maxed-out white that fights the chrome.
    //   2. There's now a clear typographic step between body
    //      (default) and emphasis (bright_white / bright_cyan / etc.),
    //      so **bold** and `code` actually POP. Previously body was
    //      ANSI 7 and bold was ANSI 15 — on many themes these render
    //      indistinguishably and emphasis silently disappeared.
    // Mutable so the markdown renderer is themable: set_markdown_palette()
    // (markdown.hpp) overwrites these once at startup; the render path reads
    // them live. Defaults below preserve the original look.
    inline Color text         = Color::default_color();
    inline Color heading1     = Color::slot(ThemeSlot::Link);
    inline Color heading2     = Color::slot(ThemeSlot::Info);
    inline Color heading3     = Color::slot(ThemeSlot::Primary);
    inline Color heading_dim  = Color::slot(ThemeSlot::Primary);
    inline Color heading_rule = Color::slot(ThemeSlot::Muted);
    inline Color bold_fg      = Color::slot(ThemeSlot::Text);
    // Italics shift to the muted gray slot so they read as soft
    // commentary against the default-fg body, instead of looking
    // identical to plain prose with a (terminal-dependent, often
    // invisible) italic flag.
    inline Color italic_fg    = Color::slot(ThemeSlot::Muted);
    inline Color code_fg      = Color::slot(ThemeSlot::Link);
    inline Color code_bg      = Color::slot(ThemeSlot::Surface);
    inline Color link_fg      = Color::slot(ThemeSlot::Primary);
    inline Color image_fg     = Color::slot(ThemeSlot::Accent);
    inline Color strike_fg    = Color::slot(ThemeSlot::Muted);
    inline Color quote_bar    = Color::slot(ThemeSlot::Warning);
    inline Color quote_text   = Color::default_color();
    inline Color list_bullet  = Color::slot(ThemeSlot::Primary);
    inline Color list_num     = Color::slot(ThemeSlot::Primary);
    inline Color checkbox_fg  = Color::slot(ThemeSlot::Success);
    inline Color checkbox_off = Color::slot(ThemeSlot::Muted);
    inline Color code_border  = Color::slot(ThemeSlot::Muted);
    inline Color code_lang    = Color::slot(ThemeSlot::Muted);
    inline Color hrule_fg     = Color::slot(ThemeSlot::Muted);
    inline Color footnote_fg  = Color::slot(ThemeSlot::Muted);
    inline Color table_border = Color::slot(ThemeSlot::Muted);
    inline Color table_header = Color::slot(ThemeSlot::Link);
    inline Color highlight_bg = Color::slot(ThemeSlot::Warning);
    inline Color highlight_fg = Color::slot(ThemeSlot::Surface);
    inline Color mention_fg   = Color::slot(ThemeSlot::Link);
    inline Color kbd_fg       = Color::slot(ThemeSlot::Text);
    inline Color kbd_border   = Color::slot(ThemeSlot::Muted);
    inline Color alert_note      = Color::slot(ThemeSlot::Primary);
    inline Color alert_tip       = Color::slot(ThemeSlot::Success);
    inline Color alert_important = Color::slot(ThemeSlot::Accent);
    inline Color alert_warning   = Color::slot(ThemeSlot::Warning);
    inline Color alert_caution   = Color::slot(ThemeSlot::Error);
}

namespace md_detail {

// ── text_transform.cpp ─────────────────────────────────────────────────────
// Text-node post-pass: decode HTML entities, expand :emoji:, linkify bare
// URLs, and recognize @user / #N / org/repo#N mentions. Mutates `nodes` in
// place, recursing into emphasis children. parser.cpp's parse_inlines()
// wrapper calls this once over the whole tree.
void post_process_text_nodes(std::vector<md::Inline>& nodes);

// ── html_tag.cpp ────────────────────────────────────────────────────────────
// One-tag HTML scanner shared by the inline parser (allow-listed inline tags)
// and the block parser (<details>, §4.6 HTML blocks). try_parse_html_tag
// recognizes a single <name …> / </name> / <name/> at `start`, lowercasing
// the name and capturing title/id/href; an unmatched result means "not a
// tag" so callers fall back to literal text. find_html_closer locates the
// matching </name> for a paired tag.
struct HtmlTagInfo {
    bool        matched      = false;
    bool        is_closer    = false;
    bool        self_closing = false;
    std::string name;                   // lowercased, no <, /, >, or attrs
    std::string attr_title;             // for <abbr title="...">
    std::string attr_id;                // for <a id="...">
    std::string attr_href;              // for <a href="...">
    std::size_t end          = 0;       // one past the closing '>'
};
[[nodiscard]] HtmlTagInfo try_parse_html_tag(std::string_view text, std::size_t start);
[[nodiscard]] std::size_t find_html_closer(std::string_view text, std::size_t start,
                                           std::string_view tag, std::size_t max_dist = 4000);

// ── parser.cpp ─────────────────────────────────────────────────────────────
[[nodiscard]] std::vector<md::Inline> parse_inlines(std::string_view text);
[[nodiscard]] md::Document             parse_markdown_impl(std::string_view source, int depth);
[[nodiscard]] std::string              collect_ref_defs(
    std::string_view source,
    std::unordered_map<std::string, md::LinkRef>& defs);

// List/indent helpers used by streaming.cpp's classify_blank_line and
// originally defined inside parser.cpp's anonymous namespace. Their
// bodies are trivial enough that inlining them here avoids any cross-TU
// linker plumbing.
[[nodiscard]] inline int count_indent(std::string_view line) noexcept {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '	') n += 4;
        else break;
    }
    return n;
}

[[nodiscard]] inline int ul_marker_len(std::string_view line) noexcept {
    // trim leading whitespace (matches parser.cpp::trim's leading-only
    // portion; trailing whitespace doesn't affect the marker test).
    std::size_t lead = 0;
    while (lead < line.size() &&
           (line[lead] == ' ' || line[lead] == '	')) ++lead;
    auto t = line.substr(lead);
    if (t.size() >= 2 &&
        (t[0] == '-' || t[0] == '*' || t[0] == '+') &&
        t[1] == ' ') {
        return static_cast<int>(lead) + 2;
    }
    return 0;
}

[[nodiscard]] inline int ol_marker_len(std::string_view line) noexcept {
    std::size_t lead = 0;
    while (lead < line.size() &&
           (line[lead] == ' ' || line[lead] == '	')) ++lead;
    auto t = line.substr(lead);
    if (t.size() < 3) return 0;
    std::size_t d = 0;
    while (d < t.size() &&
           static_cast<unsigned char>(t[d]) >= '0' &&
           static_cast<unsigned char>(t[d]) <= '9') ++d;
    if (d == 0 || d >= t.size()) return 0;
    if ((t[d] == '.' || t[d] == ')') &&
        d + 1 < t.size() && t[d + 1] == ' ') {
        return static_cast<int>(lead) + static_cast<int>(d) + 2;
    }
    return 0;
}

// RAII scope for the thread-local reference-link map that parse_inlines
// consults when resolving `[text][label]` and `[label]` references. The
// original code (single-TU) used an anonymous-namespace RefDefsGuard;
// since streaming.cpp lives in a different TU now and needs the same
// behaviour during commit_range, this is the published form.
struct RefDefsScope {
    const std::unordered_map<std::string, md::LinkRef>* prev;
    explicit RefDefsScope(const std::unordered_map<std::string, md::LinkRef>* p) noexcept;
    ~RefDefsScope();
    RefDefsScope(const RefDefsScope&) = delete;
    RefDefsScope& operator=(const RefDefsScope&) = delete;
};

// SIMD-friendly newline search (memchr under the hood). Used by parser
// internals AND the syntax highlighter.
[[nodiscard]] std::size_t find_eol(const char* data,
                                   std::size_t start,
                                   std::size_t end) noexcept;

// ── syntax.cpp ─────────────────────────────────────────────────────────────
//
// Highlight a code block. Memoised by (lang_tag, code) FNV-1a hash —
// re-rendering the same fence after the cache is warm is a hash lookup.
[[nodiscard]] Element highlight_code(const std::string& code,
                                     const std::string& lang_tag);

// ── render.cpp ─────────────────────────────────────────────────────────────
//
// Flatten an inline AST span into a (content, runs) pair attached to the
// passed-in buffers. Hot path during streaming — called for every tail
// frame and every committed block.
void                  flatten_inline(const md::Inline& span,
                                     const Style& inherited,
                                     std::string& out,
                                     std::vector<StyledRun>& runs);
// Flatten a span run, interpreting interleaved inline raw-HTML tags as a
// style stack (delegates HTML semantics to maya::html). Prefer this over the
// single-span flatten_inline when rendering a whole inline sequence.
void                  flatten_inlines(const std::vector<md::Inline>& spans,
                                      const Style& base,
                                      std::string& out,
                                      std::vector<StyledRun>& runs);
[[nodiscard]] Element build_inline_row(const std::vector<md::Inline>& spans);
[[nodiscard]] int     measure_inline_width(const std::vector<md::Inline>& spans);
[[nodiscard]] Element render_list(const md::List& l, int depth);

// streaming.cpp's assemble_markdown / commit_range walk a Document and
// call into md_block_to_element via the public API in markdown.hpp; this
// thunk simply re-exposes it for symmetry / discoverability and isn't
// strictly needed.
[[nodiscard]] Element assemble_markdown(md::Document&& doc);

} // namespace md_detail
} // namespace maya
