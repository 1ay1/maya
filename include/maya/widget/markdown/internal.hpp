// internal.hpp — shared declarations across the markdown TUs.
//
// The original monolithic widget/markdown.cpp was carved along its natural
// boundaries into parser.cpp / syntax.cpp / render.cpp / streaming.cpp.
// Symbols those TUs need to call across the boundary live here, in the
// internal `maya::md_detail` namespace. Private to the implementation —
// NOT installed and NOT included by public consumers.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/style/theme.hpp"   // Theme + slots, for the palette projection
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
// ============================================================================
// The markdown palette — ONE authored mapping, published atomically
// ============================================================================
// MAYA_MD_PALETTE below is the single statement of "which theme slot does
// each markdown role use". The MarkdownPalette struct, the projection from a
// Theme, and the defaults are all generated from it.
//
// It used to be authored TWICE — these defaults, and markdown_palette_from()
// in render_block.cpp — and the two disagreed on NINE of thirty-seven roles
// (heading1/2/3, table_header, highlight_fg, code_border, hrule_fg, code_fg,
// mention_fg). So markdown rendered under mapping A until the first theme
// swap and mapping B forever after: headings, code spans and links visibly
// shifted colour on a swap that changed nothing else, and swapping to the
// SAME theme still moved them. Two hand-written 37-field lists cannot be
// kept in agreement by review; one list cannot disagree with itself.
//
// ── Why a snapshot behind an atomic ────────────────────────────────────
//
// These were 35 plain mutable globals, written by the UI thread in
// set_markdown_palette() and read by the markdown render path — which
// includes a DETACHED std::thread doing the streaming re-parse (see
// streaming/async.cpp), across ~124 reads in render_block/render_inline/
// render_tail. That is a data race by construction, and the appearance
// panel has no streaming gate, so live-previewing a theme mid-response is
// exactly the interaction that triggers it.
//
// Now the palette is an IMMUTABLE value published by pointer swap: readers
// take one acquire load and then read a frozen object that no one will ever
// mutate. The race is gone structurally rather than by locking the hot path.

#define MAYA_MD_PALETTE(X)                                                    \
    /* field            theme slot        */                                  \
    X(text,             Text)                                                 \
    X(heading1,         Link)                                                 \
    X(heading2,         Info)                                                 \
    X(heading3,         Primary)                                              \
    X(heading_dim,      Primary)                                              \
    X(heading_rule,     Muted)                                                \
    X(bold_fg,          Text)                                                 \
    /* Italic keeps its muted step: it reads as soft commentary against the */\
    /* body rather than relying on an italic flag many terminals drop.      */\
    X(italic_fg,        Muted)                                                \
    X(code_fg,          Link)                                                 \
    /* Surface, not black: a literal black background is a hole punched in  */\
    /* a light scheme. Surface is defined as "one step off the canvas".     */\
    X(code_bg,          Surface)                                              \
    X(link_fg,          Primary)                                              \
    X(image_fg,         Accent)                                               \
    X(strike_fg,        Muted)                                                \
    X(quote_bar,        Warning)                                              \
    X(quote_text,       Text)                                                 \
    X(list_bullet,      Primary)                                              \
    X(list_num,         Primary)                                              \
    X(checkbox_fg,      Success)                                              \
    X(checkbox_off,     Muted)                                                \
    X(code_border,      Muted)                                                \
    X(code_lang,        Muted)                                                \
    X(hrule_fg,         Muted)                                                \
    X(footnote_fg,      Muted)                                                \
    X(table_border,     Muted)                                                \
    X(table_header,     Link)                                                 \
    X(highlight_bg,     Highlight)                                            \
    X(highlight_fg,     Surface)                                              \
    X(mention_fg,       Link)                                                 \
    X(kbd_fg,           Text)                                                 \
    X(kbd_border,       Muted)                                                \
    X(alert_note,       Primary)                                              \
    X(alert_tip,        Success)                                              \
    X(alert_important,  Accent)                                               \
    X(alert_warning,    Warning)                                              \
    X(alert_caution,    Error)

namespace colors {

// The palette as a value. Immutable once published.
//
// LitColor, not Color: this is the PAINTED palette, projected from a theme
// that has already resolved its slots. A renderer reading `colors::text`
// gets something it can emit, with no "did anyone resolve this" question
// left — and the parse worker, which cannot reach a Theme, does not need to.
struct Palette {
#define X(f, SLOT) LitColor f;
    MAYA_MD_PALETTE(X)
#undef X
    // Publishing compares before appending, so re-publishing the palette
    // already in force costs a compare rather than a snapshot per frame.
    constexpr bool operator==(const Palette&) const = default;
};

// Project a theme through the one mapping above.
[[nodiscard]] constexpr Palette project(const Theme& t) noexcept {
    Palette p{};
#define X(f, SLOT) p.f = t.resolve(Color::slot(ThemeSlot::SLOT));
    MAYA_MD_PALETTE(X)
#undef X
    return p;
}

// Published snapshots, append-only and NEVER freed.
//
// A two-buffer flip is not enough, and the test caught it: with slots A and
// B, the third publish overwrites A — which a reader that loaded A and was
// descheduled is still reading. It observes half of one theme and half of
// another, which is exactly the tearing this design exists to remove. (151
// torn reads out of 929, with four readers and 57 themes cycling.)
//
// So a publish never reuses storage: it leaks a new immutable snapshot and
// retires the old pointer. A reader's pointer therefore stays valid and
// UNWRITTEN forever, which is what lets readers run with no epoch, no hazard
// pointer and no lock.
//
// "Leak" is meant literally, and it has to be. An earlier attempt kept the
// snapshots in a deque and let it own them; TSan still flagged a race,
// because a container that ever FREES a node hands that memory back to the
// allocator, which reuses it for the NEXT snapshot — and the allocator's
// write into it races the reader still holding the old pointer. The address
// is what must be immortal, not merely the object.
//
// The cost is bounded by how many times a human picks a theme in one
// session: each snapshot is ~35 * 4 bytes, so a pathological 10,000 swaps
// is under 1.5 MB and the realistic figure is a few hundred bytes.
// Reclaiming them safely would need epochs or RCU to know when the last
// reader is done — a large amount of machinery to buy back nothing that
// matters at this scale.
namespace detail {
inline std::mutex& publish_mu() {
    static std::mutex m;
    return m;
}
inline std::atomic<const Palette*>& live_slot() noexcept {
    // Seeded with native's projection so the boot palette IS the projection
    // — the divergence that made the first theme swap repaint prose that
    // nothing had actually restyled.
    static std::atomic<const Palette*> p{new Palette{project(theme::native)}};
    return p;
}
}  // namespace detail

/// The palette in force. One acquire load; the result is frozen and stays
/// valid for as long as the caller holds it.
[[nodiscard]] inline const Palette& live() noexcept {
    return *detail::live_slot().load(std::memory_order_acquire);
}

/// Publish a new palette. Readers never block and never see a partial one.
inline void publish(const Palette& p) {
    // Serialises publishers against each other only; readers take the
    // atomic load and never touch this.
    std::lock_guard lk(detail::publish_mu());
    // No-op when nothing moved. Hosts re-publish every frame so `auto` can
    // follow a tmux detach, and without this that would leak a snapshot per
    // frame rather than one per actual theme change.
    if (live() == p) return;
    detail::live_slot().store(new Palette{p}, std::memory_order_release);
}

// Field accessors, so ~124 existing `colors::text` reads keep working
// unchanged while going through the atomic. Functions rather than
// references because the target moves on publish.
#define X(f, SLOT) [[nodiscard]] inline LitColor f() noexcept { return live().f; }
MAYA_MD_PALETTE(X)
#undef X

}  // namespace colors

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
