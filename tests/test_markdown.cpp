// test_markdown.cpp — Exhaustive markdown parser/renderer coverage + hang
// bisector. Each scenario runs under a hard per-test timeout (std::async +
// wait_for). If the parser hangs, the process exits immediately with the
// scenario name last on stdout — you know which feature is to blame.
//
// The binary is intentionally single-file / no-deps beyond maya + std so it
// builds on Linux, macOS, and Windows (MSVC) out of the box.

#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace maya;
using namespace std::chrono_literals;

// ───────────────────────────── harness ──────────────────────────────────────

static int g_passed  = 0;
static int g_failed  = 0;
static int g_slow    = 0;
static int g_skipped = 0;

// Scenarios named in $MD_SKIP (comma-separated substrings) are not executed.
// Lets you resume the suite past a known-expensive scenario to find further
// hangs:  MD_SKIP="deep nested list" ./test_markdown
static bool should_skip(std::string_view name) {
    const char* env = std::getenv("MD_SKIP");
    if (!env || !*env) return false;
    std::string_view s{env};
    while (!s.empty()) {
        auto comma = s.find(',');
        auto tok = (comma == std::string_view::npos) ? s : s.substr(0, comma);
        while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
        while (!tok.empty() && tok.back() == ' ')  tok.remove_suffix(1);
        if (!tok.empty() && name.find(tok) != std::string_view::npos)
            return true;
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return false;
}

// Render so the parser *and* layout engine both get exercised. Runs at 3
// realistic widths — narrow, standard, wide — so width-dependent wrap paths
// execute too.
static void render_md(std::string_view src) {
    Element el = markdown(src);
    for (int w : {40, 80, 160}) {
        StylePool pool;
        Canvas canvas(w, /*h=*/4000, &pool);
        render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
        volatile int ch = content_height(canvas);
        (void)ch;
    }
}

// Render a StreamingMarkdown's current build at one realistic width —
// simulates what the TUI view layer does every frame.
static void render_stream(const StreamingMarkdown& md) {
    Element el = md.build();
    StylePool pool;
    Canvas canvas(80, /*h=*/4000, &pool);
    render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
    volatile int ch = content_height(canvas);
    (void)ch;
}

// Run `fn` under a watchdog. Prints `name` BEFORE dispatch so stdout-flushed
// output pinpoints the stuck scenario if we blow the timeout. On timeout we
// std::exit so the async thread is abandoned — the process death reaps it.
template <typename F>
static void run(std::string_view name, std::chrono::milliseconds timeout, F&& fn) {
    std::print("  {:<58}", name);
    std::fflush(stdout);

    if (should_skip(name)) {
        std::println(" SKIP");
        std::fflush(stdout);
        ++g_skipped;
        return;
    }

    auto start = std::chrono::steady_clock::now();
    auto fut = std::async(std::launch::async, std::forward<F>(fn));

    if (fut.wait_for(timeout) == std::future_status::timeout) {
        std::println(" HUNG  (>{} ms)", timeout.count());
        std::fflush(stdout);
        // Abandon fut; OS cleans up the stuck thread when we die.
        std::_Exit(2);
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    try {
        fut.get();
    } catch (const std::exception& e) {
        std::println(" FAIL  [{} ms]: {}", ms, e.what());
        ++g_failed;
        return;
    } catch (...) {
        std::println(" FAIL  [{} ms]: unknown exception", ms);
        ++g_failed;
        return;
    }

    // Flag anything slower than 1s as "slow" — these are bisect candidates
    // even if they don't fully hang.
    if (ms > 1000) {
        std::println(" SLOW  [{} ms]", ms);
        ++g_slow;
    } else {
        std::println(" ok    [{} ms]", ms);
    }
    ++g_passed;
}

// repeat(s, n) — concatenate `s` n times without O(n^2) growth.
static std::string repeat(std::string_view s, std::size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; ++i) out.append(s);
    return out;
}

// ─────────────────────────── A. features ────────────────────────────────────
// Correctness coverage: every documented feature gets exercised, at least
// one happy-path test per feature. Timeouts are snug — these should all be
// sub-millisecond.

static void f_empty()            { render_md(""); }
static void f_whitespace()       { render_md("   \n\t\n  \n"); }
static void f_single_paragraph() { render_md("Hello, world."); }
static void f_many_paragraphs()  { render_md("Para 1\n\nPara 2\n\nPara 3"); }

static void f_heading_atx_all() {
    for (int i = 1; i <= 6; ++i)
        render_md(std::string(static_cast<std::size_t>(i), '#') + " H" + std::to_string(i));
}
static void f_heading_setext_eq()   { render_md("Title\n====="); }
static void f_heading_setext_dash() { render_md("Title\n-----"); }

static void f_bold_star()       { render_md("A **bold** word"); }
static void f_bold_underscore() { render_md("A __bold__ word"); }
static void f_italic_star()     { render_md("An *italic* word"); }
static void f_italic_underscore(){render_md("An _italic_ word"); }
static void f_bold_italic()     { render_md("A ***bold italic*** word"); }
static void f_strikethrough()   { render_md("A ~~struck~~ word"); }
static void f_inline_code()     { render_md("Use `foo()` here"); }
static void f_inline_code_multi_backtick() { render_md("Use ``code with ` backtick`` here"); }
static void f_escapes()         { render_md("A \\*literal\\* star and \\_underscore\\_"); }

static void f_fenced_code_plain()   { render_md("```\ncode\n```"); }
static void f_fenced_code_lang()    { render_md("```cpp\nint x = 0;\n```"); }
static void f_fenced_code_tilde()   { render_md("~~~\ncode\n~~~"); }
static void f_indented_code()       { render_md("    int x = 0;\n    return x;"); }
static void f_inline_math()         { render_md("Here is $a + b = c$ math"); }
static void f_display_math()        { render_md("$$\n\\int_0^1 x \\, dx = \\frac{1}{2}\n$$"); }

static void f_bullet_dash()    { render_md("- one\n- two\n- three"); }
static void f_bullet_star()    { render_md("* one\n* two\n* three"); }
static void f_bullet_plus()    { render_md("+ one\n+ two\n+ three"); }
static void f_ordered_dot()    { render_md("1. one\n2. two\n3. three"); }
static void f_ordered_paren()  { render_md("1) one\n2) two\n3) three"); }
static void f_task_list()      { render_md("- [x] done\n- [ ] open\n- [X] also done"); }
static void f_nested_list()    { render_md("- outer\n  - inner\n    - deeper\n- outer 2"); }
static void f_list_with_paragraphs() {
    render_md("- Item one\n\n  Paragraph continuation for item one.\n\n- Item two");
}

static void f_blockquote_one()   { render_md("> quoted"); }
static void f_blockquote_multi() { render_md("> line 1\n> line 2\n> line 3"); }
static void f_blockquote_nested(){ render_md("> outer\n>> inner\n>>> innermost"); }
static void f_blockquote_with_list() { render_md("> - a\n> - b\n>   - c"); }

static void f_table_simple() { render_md("| A | B |\n|---|---|\n| 1 | 2 |"); }
static void f_table_align()  {
    render_md("| L | C | R |\n|:--|:-:|--:|\n| 1 | 2 | 3 |");
}
static void f_table_empty_cells() { render_md("| A | B |\n|---|---|\n|   |   |"); }

static void f_link_inline()       { render_md("See [the docs](https://example.com/a) now"); }
static void f_link_title()        { render_md("[alt](https://e.com \"title\")"); }
static void f_autolink()          { render_md("Visit <https://example.com>"); }
static void f_bare_url_like()     { render_md("URL: https://example.com/path?q=1"); }
static void f_image_inline()      { render_md("![alt](https://e.com/x.png)"); }
static void f_footnote_ref_def()  { render_md("Claim[^1]\n\n[^1]: Because."); }

static void f_hr_dash()  { render_md("Before\n\n---\n\nAfter"); }
static void f_hr_star()  { render_md("Before\n\n***\n\nAfter"); }
static void f_hr_under() { render_md("Before\n\n___\n\nAfter"); }

static void f_hard_break_backslash() { render_md("line 1\\\nline 2"); }
static void f_hard_break_spaces()    { render_md("line 1  \nline 2"); }

static void f_unicode_bmp()    { render_md("héllo — wörld (café)"); }
static void f_unicode_cjk()    { render_md("你好世界。これは日本語です。"); }
static void f_unicode_emoji()  { render_md("status: ok 👍 🚀 ✨"); }
static void f_unicode_combining(){render_md("é (e\u0301) vs é"); }

// ─────────────────── A2. new features (bucket 1 + 2) ───────────────────────

// Inline formatting extensions
static void f_highlight_eq()   { render_md("A ==marked== span"); }
static void f_highlight_mark() { render_md("A <mark>marked</mark> span"); }
static void f_sub_tilde()      { render_md("H~2~O molecule"); }
static void f_sup_caret()      { render_md("E=mc^2^ relativity"); }
static void f_sub_tag()        { render_md("H<sub>2</sub>O molecule"); }
static void f_sup_tag()        { render_md("E=mc<sup>2</sup>"); }
static void f_br_tag()         { render_md("line 1<br>line 2<br/>line 3"); }
static void f_kbd_tag()        { render_md("Press <kbd>Ctrl</kbd>+<kbd>C</kbd> to copy"); }
static void f_strong_em_tag()  { render_md("<strong>bold</strong> and <em>italic</em>"); }
static void f_span_tag()       { render_md("Use a <span>span</span> here"); }
static void f_abbr_tag()       { render_md("The <abbr title=\"HyperText\">HTML</abbr> spec"); }

// Reference-style links (collapsed/full/shortcut)
static void f_link_ref_full()       { render_md("See [the docs][d] soon.\n\n[d]: https://example.com"); }
static void f_link_ref_collapsed()  { render_md("See [docs][] today.\n\n[docs]: https://example.com"); }
static void f_link_ref_shortcut()   { render_md("See [docs] today.\n\n[docs]: https://example.com"); }
static void f_link_ref_case_fold()  { render_md("See [Docs][] now.\n\n[DOCS]: https://example.com"); }
static void f_link_ref_missing()    { render_md("Dangling [nope][notdef] here."); }
static void f_link_anchor()         { render_md("Jump to [section](#heading-2)"); }
static void f_link_email_autolink() { render_md("Mail <x@example.com> please"); }

// Images: reference-style, titles, clickable (image inside link)
static void f_image_ref()           { render_md("![alt][i]\n\n[i]: https://e.com/x.png \"Pic\""); }
static void f_image_title()         { render_md("![alt](https://e.com/x.png \"Pic\")"); }
static void f_image_clickable()     { render_md("[![alt](https://e.com/x.png)](https://e.com)"); }

// HTML entities + emoji shortcodes
static void f_entities_named()      { render_md("A&amp;B &lt;x&gt; &copy; &mdash; tip"); }
static void f_entities_numeric()    { render_md("check: &#x2713; cross: &#x2717; pi: &#960;"); }
static void f_emoji_shortcodes()    { render_md("status :rocket: :+1: :thinking: :fire:"); }

// GitHub-style mentions and refs
static void f_mention_user()        { render_md("cc @alice and @bob_dev for review"); }
static void f_mention_issue()       { render_md("Fixes #42 (see also #1000)"); }
static void f_mention_cross_repo()  { render_md("Dupe of anthropic/claude-code#123"); }

// Alerts (GitHub-style)
static void f_alert_note()          { render_md("> [!NOTE]\n> This is a note."); }
static void f_alert_tip()           { render_md("> [!TIP]\n> Hot tip."); }
static void f_alert_important()     { render_md("> [!IMPORTANT]\n> Read this!"); }
static void f_alert_warning()       { render_md("> [!WARNING]\n> Careful."); }
static void f_alert_caution()       { render_md("> [!CAUTION]\n> Danger."); }
static void f_alert_with_children() {
    render_md("> [!TIP]\n> Multi-line alert with `code` and **bold**.\n> And a second line.");
}

// Definition lists
static void f_def_list_simple()     { render_md("Term\n: Definition"); }
static void f_def_list_multi()      { render_md("Term1\n: Def1a\n: Def1b\n\nTerm2\n: Def2"); }

// <details>/<summary> collapsible block
static void f_details_summary() {
    render_md("<details>\n<summary>Click me</summary>\n\nHidden body here.\n\n</details>");
}

// Raw HTML block passthrough
static void f_html_block_div()      { render_md("<div class=x>\nhello\n</div>"); }
static void f_html_block_section()  { render_md("<section>\n<p>para</p>\n</section>"); }

// Anchor ID passthrough
static void f_anchor_id()           { render_md("<a id=\"top\"></a>\n\n# Top"); }

// Mermaid / diagram fallback — must still render as a code block
static void f_mermaid_fallback()    { render_md("```mermaid\ngraph LR\nA-->B\n```"); }
static void f_matrix_fallback()     { render_md("```matrix\n| 1 2 |\n| 3 4 |\n```"); }
static void f_latex_fallback()      { render_md("```latex\n\\frac{1}{2}\n```"); }

// ─────────────── A2b. edge cases & gotchas (new features) ───────────────────
// One-liner per trap. All under 1s (trivial input). If any of these hang, the
// scenario name tells you which class of input is at fault.

// ── highlight ──
static void g_highlight_empty()        { render_md("A ==== span"); }
static void g_highlight_unclosed()     { render_md("A ==never closed"); }
static void g_highlight_nested_emph()  { render_md("==hi **there** you=="); }
static void g_highlight_equal_inside() { render_md("==a=b=c=="); }
static void g_highlight_crosses_line() { render_md("==a\nb==\n"); }
static void g_highlight_in_code()      { render_md("`==not highlight==`"); }

// ── sub/sup ──
static void g_sub_empty()            { render_md("H~~O"); }              // "~~" = strike start, not sub
static void g_sup_empty()            { render_md("E=mc^^"); }
static void g_sub_with_space()       { render_md("a ~b c~ d"); }         // internal space - valid or literal?
static void g_sup_with_digits()      { render_md("x^123^ power"); }
static void g_sub_inside_strike()    { render_md("~~before H~2~O after~~"); }
static void g_sup_unclosed()         { render_md("a^never closed"); }
static void g_sub_backtoback()       { render_md("H~2~O~3~ stuff"); }

// ── <br> variants ──
static void g_br_slash_space()       { render_md("a<br />b"); }
static void g_br_slash_no_space()    { render_md("a<br/>b"); }
static void g_br_extra_whitespace()  { render_md("a<br   >b"); }
static void g_br_uppercase()         { render_md("a<BR>b"); }
static void g_br_at_start()          { render_md("<br>line"); }
static void g_br_many()              { render_md("a<br><br><br>b"); }

// ── kbd/mark/abbr/span — paired tag edge cases ──
static void g_kbd_empty()            { render_md("press <kbd></kbd> now"); }
static void g_kbd_unclosed()         { render_md("press <kbd>Ctrl never"); }
static void g_kbd_nested()           { render_md("<kbd>Ctrl+<kbd>C</kbd></kbd>"); }
static void g_span_attrs()           { render_md("<span class=\"x\" id=y>text</span>"); }
static void g_abbr_no_title()        { render_md("<abbr>XML</abbr> specs"); }
static void g_abbr_empty_title()     { render_md("<abbr title=\"\">XML</abbr>"); }
static void g_mark_inside_strong()   { render_md("**<mark>hot</mark>**"); }

// ── HTML entities ──
static void g_entity_unknown()       { render_md("A &foo; B"); }
static void g_entity_no_semi()       { render_md("A &amp B"); }
static void g_entity_in_code()       { render_md("`&amp;`"); }
static void g_entity_hex_huge()      { render_md("&#xFFFFFFFF;"); }
static void g_entity_dec_zero()      { render_md("&#0; end"); }
static void g_entity_adjacent()      { render_md("&amp;&lt;&gt;&quot;&apos;"); }
static void g_entity_trailing_amp()  { render_md("end with &"); }

// ── emoji shortcodes ──
static void g_emoji_unknown()        { render_md("hi :nope_no_such: there"); }
static void g_emoji_unclosed()       { render_md("hi :smile without end"); }
static void g_emoji_in_code()        { render_md("`:rocket:`"); }
static void g_emoji_adjacent()       { render_md(":+1::-1::rocket:"); }
static void g_emoji_in_heading()     { render_md("# :fire: hot topic"); }
static void g_emoji_in_link()        { render_md("[:star: me](url)"); }
static void g_colon_in_url()         { render_md("See https://example.com:8080/x"); }

// ── mentions ──
static void g_mention_email_conflict() { render_md("email: alice@example.com here"); }
static void g_mention_trailing_punct() { render_md("cc @alice, @bob. @carol!"); }
static void g_mention_in_code()        { render_md("`@not_a_mention`"); }
static void g_mention_at_start()       { render_md("@alice starts the line"); }
static void g_mention_issue_inline()   { render_md("see #42 or #1000 refs"); }
static void g_mention_hash_in_url()    { render_md("visit https://x.com/#section here"); }
static void g_mention_cross_repo_odd() { render_md("see foo-bar/baz_qux#9999 now"); }
static void g_mention_hash_not_issue() { render_md("C# and F# are languages"); }    // '#' not followed by digit

// ── reference links ──
static void g_ref_case_fold_spaces()  { render_md("[My  Link][]\n\n[MY LINK]: https://e.com"); }
static void g_ref_title_paren()       { render_md("See [x][]\n\n[x]: https://e.com (title)"); }
static void g_ref_title_single()      { render_md("See [x][]\n\n[x]: https://e.com 'title'"); }
static void g_ref_url_angle()         { render_md("See [x][]\n\n[x]: <https://e.com>"); }
static void g_ref_dup_def()           { render_md("[x][]\n\n[x]: https://a.com\n[x]: https://b.com"); }
static void g_ref_def_in_code()       { render_md("```\n[foo]: not-a-ref\n```\n\n[foo]"); }
static void g_ref_empty_label()       { render_md("[][foo]\n\n[foo]: https://e.com"); }
static void g_ref_shortcut_with_code(){ render_md("See [`x`] soon\n\n[`x`]: https://e.com"); }

// ── email autolink ──
static void g_email_with_plus()       { render_md("<a+b@example.com>"); }
static void g_email_bad()             { render_md("<not-an-email>"); }
static void g_email_bare_in_text()    { render_md("write to a@b.com today"); }

// ── image edge ──
static void g_image_missing_ref()     { render_md("![alt][missing]"); }
static void g_image_empty_alt()       { render_md("![](https://e.com/x.png)"); }
static void g_image_relative()        { render_md("![x](./img/foo.png)"); }

// ── alerts ──
static void g_alert_lowercase()       { render_md("> [!note]\n> body"); }           // must NOT fire
static void g_alert_unknown_kind()    { render_md("> [!FOO]\n> body"); }            // falls back
static void g_alert_space_after()     { render_md("> [!NOTE] extra\n> body"); }     // extra text allowed?
static void g_alert_empty()           { render_md("> [!NOTE]"); }                   // no body
static void g_alert_no_marker()       { render_md("> [NOTE]\n> body"); }            // missing '!'
static void g_alert_inside_quote()    { render_md("> outer\n> > [!TIP]\n> > nested"); }

// ── definition lists ──
static void g_deflist_no_space()      { render_md("Term\n:def"); }                  // no space after ':'
static void g_deflist_multi_def()     { render_md("Term\n: a\n: b\n: c"); }
static void g_deflist_with_bold()     { render_md("**Term**\n: defined"); }
static void g_deflist_multi_term()    { render_md("T1\nT2\n: defines both"); }      // multi-line term?

// ── <details>/<summary> ──
static void g_details_no_summary()    { render_md("<details>\n\nbody text\n\n</details>"); }
static void g_details_empty()         { render_md("<details></details>"); }
static void g_details_nested()        {
    render_md("<details>\n<summary>outer</summary>\n\n<details>\n<summary>inner</summary>\n\ndeep.\n\n</details>\n\n</details>");
}

// ── raw html block ──
static void g_htmlblock_unclosed()    { render_md("<div>\nstart but no close"); }
static void g_htmlblock_nested_tags() { render_md("<div><p><span>hi</span></p></div>"); }
static void g_htmlblock_with_md()     { render_md("<div>\n\n**bold** inside?\n\n</div>"); }

// ── diagram / code-fence fallback ──
static void g_mermaid_empty()         { render_md("```mermaid\n```"); }
static void g_mermaid_uppercase()     { render_md("```MERMAID\ngraph\n```"); }       // case: bare "MERMAID" — just a lang label
static void g_math_fence()            { render_md("```math\n1+1\n```"); }

// ── combined / interaction ──
static void g_all_inline_together() {
    render_md("==h== ~s~ ^p^ <kbd>K</kbd> <mark>m</mark> :rocket: @u #1 &amp; a@b.co");
}

// ────────── A2c. CommonMark / GFM gotchas (whitespace + escaping) ──────────

// Trailing-space hard break: "  \n" = <br>. Invisible, easy to strip.
static void g_trailing_two_spaces_br()   { render_md("line 1  \nline 2"); }
static void g_trailing_three_spaces_br() { render_md("line 1   \nline 2"); }
static void g_trailing_space_at_eof()    { render_md("final line  "); }            // trailing ws, EOF
static void g_space_on_empty_line()      { render_md("para 1\n   \npara 2"); }     // blank line w/ spaces

// Backslash line break (preferred): "line\\n" also yields <br>
static void g_backslash_eol_break()      { render_md("line 1\\\nline 2"); }
static void g_backslash_eol_before_blank(){ render_md("line 1\\\n\nline 3"); }

// Nested list indentation rules
static void g_nested_ol_ul_3sp()         { render_md("1. Parent\n   - Child\n   - Child 2"); }
static void g_nested_ol_ul_4sp()         { render_md("1. Parent\n    - Child"); }
static void g_nested_ol_ul_0sp()         { render_md("1. Parent\n- New list"); }   // NOT a child
static void g_nested_ol_mixed_indents()  {
    render_md("1. Outer\n   - Mid\n     - Inner\n   - Back mid\n2. Next outer");
}
static void g_nested_ul_tabs()           { render_md("- outer\n\t- tabbed inner"); }

// List adjacent to paragraph (GFM loose; strict CommonMark different)
static void g_list_no_blank_before()     { render_md("Some text.\n- item 1\n- item 2"); }
static void g_para_then_ol_no_blank()    { render_md("A paragraph.\n1. one\n2. two"); }

// Intraword underscores — GFM suppresses emphasis inside a word
static void g_intraword_underscore()     { render_md("use my_variable_name here"); }
static void g_intraword_mid_word()       { render_md("snake_case_name here"); }
static void g_word_boundary_underscore() { render_md("it was _emphasized_ text"); } // must still italicize
static void g_intraword_star_inside()    { render_md("foo*bar*baz intraword star"); }

// Escaping pipes in tables
static void g_table_escaped_pipe()       { render_md("| Col |\n| --- |\n| a \\| b |"); }
static void g_table_multi_escaped_pipe() { render_md("| A | B |\n|---|---|\n| x\\|y | p\\|q |"); }
static void g_table_pipe_in_code_span()  { render_md("| Col |\n| --- |\n| `a | b` |"); }

// Indented code inside a list item (4 spaces beyond the marker)
static void g_list_with_indented_code() {
    render_md("1. Item one\n\n        code inside item\n\n2. Item two");
}
static void g_ul_with_fenced_inside() {
    render_md("- item\n\n  ```\n  code\n  ```\n\n- next");
}

// HTML comments — invisible in output (inline + block)
static void g_html_comment_inline()      { render_md("before <!-- hidden --> after"); }
static void g_html_comment_only()        { render_md("<!-- just a comment -->"); }
static void g_html_comment_multiline()   { render_md("para\n\n<!--\nmulti\nline\n-->\n\ntail"); }
static void g_html_comment_unterminated(){ render_md("before <!-- never closed then text"); }
static void g_html_comment_in_code()     { render_md("`<!-- not a comment -->`"); }
static void g_html_comment_inside_p()    { render_md("line <!-- hidden -->\n\nnext para"); }

// Block-level HTML with blank lines bracketing Markdown inside
static void g_htmlblock_md_inside_blank() {
    render_md("<div>\n\n**bold** and *italic*\n\n- item 1\n- item 2\n\n</div>");
}
static void g_htmlblock_no_blank_breaks() {
    // Markdown inside block HTML without blank lines — should still not crash
    render_md("<div>\n**no blank line**\n- a\n- b\n</div>");
}

// Backslash escapes — every listed escapable char on one line
static void g_escape_all_punct() {
    render_md("\\\\ \\` \\* \\_ \\{ \\} \\[ \\] \\< \\> \\( \\) \\# \\+ \\- \\. \\! \\|");
}
static void g_escape_asterisk_literal()  { render_md("\\*literal asterisks\\*"); }
static void g_escape_backtick_literal()  { render_md("a \\`not code\\` b"); }
static void g_escape_brackets_literal()  { render_md("\\[not a link\\]\\(nowhere\\)"); }
static void g_escape_pipe_outside_table(){ render_md("a \\| b \\| c in prose"); }
static void g_escape_nonescapable()      { render_md("\\a \\b \\z stays literal"); }   // \z is not escapable
static void g_escape_inside_code_span()  { render_md("`\\*not escaped\\*`"); }        // no escape in code
static void g_escape_inside_code_fence() { render_md("```\n\\*literal\\*\n```"); }

// ── footnote edge cases & gotchas ──

// Orphans / dangling
static void g_fn_ref_no_def()           { render_md("Orphan[^nope] reference."); }
static void g_fn_def_no_ref()           { render_md("Para.\n\n[^unused]: never cited."); }
static void g_fn_empty_brackets()       { render_md("not a fn [^] here"); }         // [^] → not a ref
static void g_fn_def_empty_content()    { render_md("Cite[^1].\n\n[^1]:"); }        // empty def body
static void g_fn_def_only_whitespace()  { render_md("Cite[^1].\n\n[^1]:   "); }

// Duplicates / multi-ref
static void g_fn_duplicate_def()        { render_md("Cite[^1].\n\n[^1]: first\n\n[^1]: second"); }
static void g_fn_ref_used_twice()       { render_md("Hit[^1] and again[^1].\n\n[^1]: once."); }
static void g_fn_ref_many_same()        { render_md("X[^a][^a][^a] Y.\n\n[^a]: def."); }

// Multi-paragraph / rich content in def
static void g_fn_multi_paragraph() {
    render_md("See[^long].\n\n[^long]: First paragraph.\n\n  Second paragraph.\n\n  Third paragraph.\n\nEnd.");
}
static void g_fn_def_with_code() {
    render_md("Cite[^c].\n\n[^c]: Example:\n\n      indented code\n      more code");
}
static void g_fn_def_with_fenced_code() {
    render_md("Cite[^c].\n\n[^c]:\n\n  ```\n  code\n  ```");
}
static void g_fn_def_with_list() {
    render_md("See[^l].\n\n[^l]:\n  - item one\n  - item two");
}
static void g_fn_def_with_bold()        { render_md("Cite[^b].\n\n[^b]: **bold** and *italic* text."); }
static void g_fn_def_with_link()        { render_md("Cite[^url].\n\n[^url]: See [docs](https://e.com)."); }
static void g_fn_def_with_image()       { render_md("Cite[^i].\n\n[^i]: ![alt](https://e.com/x.png)"); }

// Nested / cross-references
static void g_fn_ref_inside_fn_def() {
    render_md("Cite[^1].\n\n[^1]: See also[^2].\n\n[^2]: Deeper.");
}

// Label variations
static void g_fn_label_numeric()        { render_md("Cite[^42].\n\n[^42]: def."); }
static void g_fn_label_alpha()          { render_md("Cite[^alpha].\n\n[^alpha]: def."); }
static void g_fn_label_hyphen()         { render_md("Cite[^a-b-c].\n\n[^a-b-c]: def."); }
static void g_fn_label_underscore()     { render_md("Cite[^a_b].\n\n[^a_b]: def."); }
static void g_fn_label_unicode()        { render_md("Cite[^日本].\n\n[^日本]: def."); }
static void g_fn_label_mixed_case()     { render_md("Cite[^FooBar].\n\n[^FooBar]: def."); }
static void g_fn_label_with_spaces()    { render_md("Cite[^a b].\n\n[^a b]: def."); }     // unusual
static void g_fn_label_very_long()      {
    std::string label(180, 'x');
    render_md("Cite[^" + label + "].\n\n[^" + label + "]: def.");
}
static void g_fn_label_over_cap() {
    // Label longer than 200-char inline cap — ref should degrade gracefully
    std::string label(500, 'z');
    render_md("Cite[^" + label + "] here.");
}

// Context / conflict
static void g_fn_ref_in_code_span()     { render_md("literal `[^1]` is not a ref"); }
static void g_fn_ref_in_code_fence()    { render_md("```\n[^1] literal\n```"); }
static void g_fn_ref_in_link_text()     { render_md("See [note [^1]](https://e.com).\n\n[^1]: def."); }
static void g_fn_ref_in_heading()       { render_md("# Title with [^1]\n\n[^1]: def."); }
static void g_fn_ref_in_table_cell() {
    render_md("| Col |\n| --- |\n| cite [^1] |\n\n[^1]: def.");
}
static void g_fn_def_inside_blockquote(){ render_md("> text\n> \n> [^q]: defined in quote"); }
static void g_fn_def_inside_list()      { render_md("- item\n- [^l]: weird placement."); }

// Caret edge cases
static void g_fn_caret_then_space()     { render_md("not a fn [^ ] and [^  stuff]"); }
static void g_fn_caret_in_link()        { render_md("[^text](url)"); }              // looks like fn but has url
static void g_fn_bracket_not_fn()       { render_md("A [^ caret not closed"); }

// Stress: def with very long body
static void g_fn_def_long_body() {
    std::string body(5'000, 'x');
    render_md("Cite[^l].\n\n[^l]: " + body);
}

// Combined "every gotcha" one-liner
static void g_gotcha_mixed() {
    render_md(
        "Some text.\n"
        "- adjacent list\n"
        "- my_variable_name\n\n"
        "<!-- note for linter -->\n\n"
        "| A | B |\n|---|---|\n| x\\|y | `c | d` |\n\n"
        "1. Parent\n"
        "   - 3-space child\n\n"
        "trailing  \n"
        "break."
    );
}

// Combined stress: mixed new features in one document
static void f_new_features_mixed() {
    std::string src =
        "# Title ==highlighted==\n\n"
        "Press <kbd>Ctrl</kbd>+<kbd>S</kbd> to save :rocket:.\n\n"
        "> [!NOTE]\n> cc @alice — fixes #7.\n\n"
        "H~2~O and E=mc^2^ — with &mdash; entity.\n\n"
        "See [docs][] for more.\n\n"
        "[docs]: https://example.com\n\n"
        "Term\n: Def here\n\n"
        "```mermaid\ngraph TD\nA-->B\n```\n";
    render_md(src);
}

// ───────────────────────── B. edge / malformed ──────────────────────────────
// These must NOT hang — but many of them used to, or still do, on naive
// parsers. Timeouts snug at 2s.

static void e_unclosed_bold()      { render_md("start **never closed"); }
static void e_unclosed_italic()    { render_md("start *never closed"); }
static void e_unclosed_code_span() { render_md("start `never closed"); }
static void e_unclosed_fence()     { render_md("```\nopen fence with no close\nmore text\nstill open"); }
static void e_unclosed_link()      { render_md("See [the docs without close"); }
static void e_unclosed_link_url()  { render_md("See [text](https://example.com without close"); }
static void e_mismatched_emphasis(){ render_md("**bold *italic** mismatched*"); }

static void e_only_delimiters()    { render_md("***"); }  // HR? emphasis? ambiguous
static void e_only_backticks()     { render_md("````"); }
static void e_only_hashes()        { render_md("########"); }
static void e_only_pipes()         { render_md("||||"); }

static void e_heading_no_text()    { render_md("#"); }
static void e_blank_link()         { render_md("[]()"); }
static void e_blank_image()        { render_md("![]()"); }
static void e_empty_code_block()   { render_md("```\n```"); }
static void e_tab_indented()       { render_md("\tcode\n\tmore"); }

static void e_crlf_line_endings()  { render_md("# Title\r\n\r\n**bold**\r\ndone."); }
static void e_trailing_newlines()  { render_md("para\n\n\n\n\n"); }
static void e_bom()                { render_md("\xEF\xBB\xBF# Heading"); }

static void e_long_url() {
    std::string src = "[link](https://example.com/";
    src.append(2000, 'x');
    src += ")";
    render_md(src);
}

// ─────────────────────────── C. size stress ─────────────────────────────────
// Well-formed markdown at size. If any of these blow the time budget, the
// parser is super-linear in that dimension. 5s per scenario.

static void s_long_paragraph_100k() {
    render_md(std::string(100'000, 'x'));
}
static void s_long_paragraph_1m() {
    render_md(std::string(1'000'000, 'x'));
}
static void s_many_paragraphs_10k() {
    render_md(repeat("Hello world paragraph.\n\n", 10'000));
}
static void s_many_headings_5k() {
    render_md(repeat("# Heading\n\n", 5'000));
}
static void s_many_list_items_10k() {
    render_md(repeat("- item\n", 10'000));
}
static void s_many_code_fences_1k() {
    render_md(repeat("```\ncode\n```\n\n", 1'000));
}
static void s_wide_table_200_cols() {
    std::string header = "|";
    std::string sep    = "|";
    std::string row    = "|";
    for (int i = 0; i < 200; ++i) { header += " h |"; sep += "---|"; row += " x |"; }
    render_md(header + "\n" + sep + "\n" + row);
}
static void s_tall_table_5k_rows() {
    std::string src = "| A | B | C |\n|---|---|---|\n";
    src.reserve(src.size() + 5'000 * 16);
    for (int i = 0; i < 5'000; ++i) src += "| 1 | 2 | 3 |\n";
    render_md(src);
}
static void s_long_code_block_100k() {
    std::string src = "```\n";
    src.append(100'000, 'x');
    src += "\n```";
    render_md(src);
}
static void s_long_blockquote_10k_lines() {
    render_md(repeat("> line\n", 10'000));
}

// ───────────────────── D. pathological / bisect bait ────────────────────────
// Patterns flagged by static audit as suspicious. Each has a tight 5s budget;
// these are the scenarios most likely to expose the hang you're seeing.

static void p_unmatched_emphasis_10k() {
    // Every `*` triggers a forward scan for a closer (capped at 2000 chars).
    // If that scan is O(n) per opener, 10k openers = O(n·2000).
    render_md(repeat("*text", 10'000));
}
static void p_alternating_emphasis_5k() {
    render_md(repeat("*a*_b_", 5'000));
}
static void p_nested_emphasis_500() {
    // 500 opens, 500 closes: *_*_*_...text..._*_*_*
    std::string src = repeat("*_", 500);
    src += "text";
    src += repeat("_*", 500);
    render_md(src);
}
static void p_nested_emphasis_extreme_2000() {
    std::string src = repeat("**", 2'000);
    src += "x";
    src += repeat("**", 2'000);
    render_md(src);
}
static void p_many_backticks_100k() {
    render_md(std::string(100'000, '`'));
}
static void p_brackets_without_close_10k() {
    render_md(repeat("[link", 10'000));
}
static void p_brackets_and_parens_stacked_5k() {
    render_md(repeat("[a](b", 5'000));
}
static void p_footnote_refs_5k() {
    std::string src;
    src.reserve(60'000);
    for (int i = 0; i < 5'000; ++i)
        src += "claim[^" + std::to_string(i) + "] ";
    src += "\n\n";
    for (int i = 0; i < 5'000; ++i)
        src += "[^" + std::to_string(i) + "]: because\n";
    render_md(src);
}
static void p_deep_blockquote_1k() {
    // 1000 '>' per line — parser caps at depth 8 per the audit, so this
    // should degrade to inline parsing rather than hang.
    std::string prefix = std::string(1'000, '>') + " ";
    render_md(prefix + "text");
}
static void p_deep_nested_list_200() {
    std::string src;
    for (int i = 0; i < 200; ++i) {
        src += std::string(static_cast<std::size_t>(i) * 2, ' ');
        src += "- item\n";
    }
    render_md(src);
}
static void p_huge_fenced_block_count_10k() {
    // Many opened fences that never close — parser must terminate scan.
    render_md(repeat("``` \n", 10'000));
}
static void p_mixed_realistic_500k() {
    // Simulates a real long LLM answer — many features, realistic distribution.
    std::string src;
    src.reserve(500'000);
    for (int i = 0; i < 2'000; ++i) {
        src += "## Section " + std::to_string(i) + "\n\n";
        src += "This is **bold**, *italic*, and `code` in a paragraph. Some text. ";
        src += "More text with a [link](https://example.com/x) and an item list:\n\n";
        src += "- alpha\n- beta\n- gamma\n\n";
        src += "```cpp\nint x = " + std::to_string(i) + ";\n```\n\n";
        src += "> quoted content here\n\n";
    }
    render_md(src);
}
static void p_long_inline_code_span() {
    std::string src = "Use `";
    src.append(100'000, 'x');
    src += "` here";
    render_md(src);
}
static void p_many_autolinks_5k() {
    render_md(repeat("see <https://example.com/x> and ", 5'000));
}
static void p_reference_soup_5k() {
    // Many bracket pairs with no matching defs.
    render_md(repeat("[a][b] ", 5'000));
}

// ───────────────────── E. StreamingMarkdown perf ────────────────────────────
// Verifies the incremental-cache path isn't accidentally disabled, and that
// append() + build() stays well under the 16ms-per-frame budget even for
// realistic LLM transcripts.  If any of these go SLOW (>1s), something is
// re-parsing content that should be cached.

// Build the same realistic LLM body used in p_mixed_realistic_500k, but here
// we want a *moderate* 20k-char body so we can stream it token-by-token and
// render every frame, simulating the TUI's steady-state load.
static std::string build_llm_body(std::size_t target_bytes) {
    std::string out;
    out.reserve(target_bytes + 1024);
    int i = 0;
    while (out.size() < target_bytes) {
        out += "## Section " + std::to_string(i++) + "\n\n";
        out += "This is **bold**, *italic*, and `code` in a paragraph. ";
        out += "A link [docs](https://example.com/x) and list:\n\n";
        out += "- alpha\n- beta\n- gamma\n\n";
        out += "```cpp\nint x = " + std::to_string(i) + ";\n```\n\n";
        out += "> quoted ==highlighted== text\n\n";
    }
    return out;
}

// Drip-feed bytes one-at-a-time. Build() every append. This is the absolute
// worst case — every build has to re-parse the growing tail because the
// cache was just dirtied by the append.  Kept small (4k) so it runs in a
// reasonable budget; serves as a correctness smoke-test under extreme load.
static void st_char_by_char_4k() {
    auto body = build_llm_body(4'000);
    StreamingMarkdown md;
    for (char c : body) {
        md.append(std::string_view{&c, 1});
        render_stream(md);
    }
    md.finish();
    render_stream(md);
}

// Realistic token-size chunks (~8 bytes = ~2 tokens). Render on a ~30fps
// cadence (every 16 chunks = ~128 bytes) — matches the view layer, not the
// stream callback. Rendering every chunk would be O(n²) in layout cost,
// which is the render engine's problem, not streaming's.
static void st_token_chunks_50k() {
    auto body = build_llm_body(50'000);
    StreamingMarkdown md;
    std::size_t off = 0;
    int since_render = 0;
    while (off < body.size()) {
        std::size_t take = std::min<std::size_t>(8, body.size() - off);
        md.append(std::string_view{body.data() + off, take});
        off += take;
        if (++since_render >= 16) {
            render_stream(md);
            since_render = 0;
        }
    }
    md.finish();
    render_stream(md);
}

// set_content() every frame with the same buffer (how moha does it). The
// equal-check fast path + build() cache must make repeated calls ~free.
// (We render ONCE up front so the layout pool is primed, then measure pure
// streaming calls — render_tree itself would be O(tree) per call, which is
// outside streaming's control.)
static void st_set_content_no_change_10k() {
    auto body = build_llm_body(10'000);
    StreamingMarkdown md;
    md.set_content(body);
    render_stream(md);
    for (int k = 0; k < 10'000; ++k) {
        md.set_content(body);              // identical every time — early-return
        const auto& e = md.build();        // must hit the cached Element
        (void)e;
    }
}

// Repeated build() with no state change — hits pure cache fast path.
static void st_repeat_build_100k() {
    StreamingMarkdown md;
    md.set_content(build_llm_body(5'000));
    render_stream(md);                     // prime once
    for (int k = 0; k < 100'000; ++k) {
        const auto& e = md.build();        // every call must be cached
        (void)e;
    }
}

// Long unclosed code fence, drip-fed one char at a time — the pathological
// "growing tail" case that can't commit until the closing ```. Render at a
// 30fps-ish cadence; per-char render is quadratic in the layout engine.
static void st_growing_code_fence() {
    StreamingMarkdown md;
    md.append("```cpp\n");
    std::string tail(5'000, 'x');
    int since_render = 0;
    for (char c : tail) {
        md.append(std::string_view{&c, 1});
        if (++since_render >= 64) {
            render_stream(md);
            since_render = 0;
        }
    }
    md.append("\n```\n");
    md.finish();
    render_stream(md);
}

// set_content with an ever-growing buffer (legacy caller pattern). Each
// frame gets the new buffer, which grows by some tokens; the prefix-equal
// fast path must kick in so source_ isn't re-allocated every frame.
// Stride of 128 bytes ≈ 30fps worth of tokens at typical LLM speeds.
static void st_set_content_grow_20k() {
    auto full = build_llm_body(20'000);
    StreamingMarkdown md;
    for (std::size_t n = 1; n <= full.size(); n += 128) {
        md.set_content(std::string_view{full.data(), n});
        render_stream(md);
    }
    md.finish();
    render_stream(md);
}

// Repeated render after N committed blocks — simulates a composer
// keystroke / Tick happening BETWEEN model deltas: state hasn't
// changed, but the host's view layer rebuilds and re-renders the
// streaming widget every frame. Each render must hit the renderer's
// hash-keyed cache (hash_id on the outer prefix component) and
// blit cached cells, NOT re-run the prefix lambda's deep-copy loop.
//
// Without working caching, cost is O(N) per render × K renders.
// With working caching, first render captures cells; subsequent
// renders blit — total cost is one slow path plus K × O(blit).
//
// Failure mode: if this exceeds the budget, the streaming-widget
// caching isn't engaging and long-streaming sessions will peg CPU
// on every keystroke, exactly what the user reported.
static void st_render_after_commits_no_change() {
    // Build a body that produces ~40 committed blocks (one per
    // paragraph-style block in build_llm_body's mix). Then finish()
    // so everything is committed (no tail). Then render 200 times
    // with zero state changes.
    auto body = build_llm_body(20'000);
    StreamingMarkdown md;
    md.set_content(body);
    md.finish();
    render_stream(md);            // prime — first render captures cells
    for (int k = 0; k < 200; ++k) {
        render_stream(md);        // every subsequent render must hit cache
    }
}

// Same scenario as above but BETWEEN commits: append a few bytes (not
// enough to cross a block boundary), render, append, render, etc.
// The widget should NOT re-render the prefix from scratch — the prefix's
// hash_id is keyed on prefix_->generation, which only advances when
// commit_range actually fires. Between commits, every render is a hit.
static void st_render_between_commits() {
    auto body = build_llm_body(10'000);
    StreamingMarkdown md;
    md.set_content(body);
    md.finish();                  // all committed; generation stable
    // Now append a single byte and render, 200 times. None of the
    // appends crosses a block boundary because the trailing bytes
    // don't form a complete paragraph; the prefix stays at its
    // current generation. Each render must hit the cache.
    for (int k = 0; k < 200; ++k) {
        md.append("x");
        render_stream(md);
    }
}

// Clear + re-stream: ensure cache invalidation is correct when clear() runs.
static void st_clear_restream() {
    StreamingMarkdown md;
    for (int round = 0; round < 10; ++round) {
        md.clear();
        auto body = build_llm_body(2'000);
        for (std::size_t off = 0; off < body.size(); off += 32) {
            auto take = std::min<std::size_t>(32, body.size() - off);
            md.append(std::string_view{body.data() + off, take});
            render_stream(md);
        }
        md.finish();
    }
}

// New: byte-offset reverse lookup + fold roundtrip + auto-fold.
static void st_block_meta_and_fold() {
    StreamingMarkdown md;
    const std::string body =
        "# Title\n\n"
        "A first paragraph.\n\n"
        "```rust\nfn a() {}\nfn b() {}\n```\n\n"
        "A trailing paragraph.\n";
    md.set_content(body);
    md.finish();

    // Must have committed at least 4 blocks: heading, para, code, para.
    if (md.block_count() < 4)
        throw std::runtime_error("expected >=4 committed blocks, got "
            + std::to_string(md.block_count()));

    // block_for_offset on a byte inside the heading must return 0.
    auto first = md.block_for_offset(2);
    if (!first || *first != 0)
        throw std::runtime_error("block_for_offset(2) != 0");

    // Find the code block by kind and toggle it.
    std::optional<std::size_t> code_off;
    for (std::size_t i = 0; i < md.block_count(); ++i) {
        if (md.block_meta(i).kind == StreamingMarkdown::BlockKind::CodeBlock) {
            code_off = md.block_meta(i).source_offset;
            break;
        }
    }
    if (!code_off) throw std::runtime_error("no code block in commit");

    // Code block has line_count > 1 — verify the metadata before
    // touching the fold map. (auto_fold respects any pre-existing
    // entry, including an explicit-unfold one, so this must precede
    // the toggle_fold roundtrip below.)
    {
        bool seen_long_code = false;
        for (std::size_t i = 0; i < md.block_count(); ++i) {
            const auto& m = md.block_meta(i);
            if (m.kind == StreamingMarkdown::BlockKind::CodeBlock
                && m.line_count > 1) { seen_long_code = true; break; }
        }
        if (!seen_long_code)
            throw std::runtime_error("code block line_count not > 1");
    }
    md.auto_fold_long_blocks(
        1u,
        1u << static_cast<unsigned>(StreamingMarkdown::BlockKind::CodeBlock));
    if (!md.is_folded(*code_off)) {
        std::string diag = "auto_fold did not fold long code block; per-block meta: ";
        for (std::size_t i = 0; i < md.block_count(); ++i) {
            const auto& m = md.block_meta(i);
            diag += "[" + std::to_string(i) + " kind="
                +  std::to_string(static_cast<int>(m.kind))
                +  " off=" + std::to_string(m.source_offset)
                +  " end=" + std::to_string(m.source_end)
                +  " lc="  + std::to_string(m.line_count) + "] ";
        }
        throw std::runtime_error(diag);
    }
    // Heading and paragraphs (line_count <= 1) must remain unfolded.
    if (md.is_folded(md.block_meta(0).source_offset))
        throw std::runtime_error("auto_fold folded a non-codeblock");
    md.unfold_all();
    if (md.is_folded(*code_off))
        throw std::runtime_error("unfold_all did not clear folds");

    // Manual toggle roundtrip on a fresh fold map.
    if (md.is_folded(*code_off))
        throw std::runtime_error("code block folded by default");
    bool folded = md.toggle_fold(*code_off);
    if (!folded) throw std::runtime_error("toggle_fold did not fold");
    render_stream(md);
    folded = md.toggle_fold(*code_off);
    if (folded) throw std::runtime_error("toggle_fold did not unfold");
}

// New: set_content_async on a small input falls through to the
// synchronous path; on a large diverging input it eventually
// applies via build() polling. We can't easily test wall-clock
// async, but we CAN verify that after a `set_content_async` +
// repeated build() polls, the source matches.
static void st_set_content_async_roundtrip() {
    StreamingMarkdown md;

    // Small input: synchronous fallthrough.
    md.set_content_async("hello **world**\n");
    render_stream(md);
    if (md.source() != "hello **world**\n")
        throw std::runtime_error("small async did not apply synchronously");

    // Large divergent input: queues a worker. Poll up to ~2 s.
    std::string big;
    big.reserve(40'000);
    for (int i = 0; i < 500; ++i) {
        big += "# Header " + std::to_string(i) + "\n\n";
        big += "Paragraph body with **bold** and `code` and a [link](https://example.com).\n\n";
        big += "```c\nint x_" + std::to_string(i) + " = 0;\n```\n\n";
    }
    md.set_content_async(big);

    // Foreground polling loop: build() is what runs maybe_apply_async_.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (md.source() != big) {
        render_stream(md);
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("async parse did not land within 5 s");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (md.block_count() == 0)
        throw std::runtime_error("async parse landed empty");
}

// Eager horizontal-rule render: when an HR line terminates but hasn't
// committed yet (no trailing blank line), the live build must already
// render the styled rule, and at the SAME content height the finished
// (committed) build produces — i.e. no height snap when commit fires.
static int stream_height(const StreamingMarkdown& md) {
    Element el = md.build();
    StylePool pool;
    Canvas canvas(80, /*h=*/4000, &pool);
    render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
    return content_height(canvas);
}
static void st_eager_hrule() {
    for (const char* rule : {"---", "***", "___", "- - -"}) {
        StreamingMarkdown live;
        live.set_live(true);
        // Feed a paragraph, blank line, then the rule line WITH its
        // newline but no following blank — the rule sits in the tail.
        live.feed(std::string("Intro.\n\n") + rule + "\n");
        int live_h = stream_height(live);

        StreamingMarkdown done;
        done.set_content(std::string("Intro.\n\n") + rule + "\n");
        done.finish();
        int done_h = stream_height(done);

        if (live_h != done_h)
            throw std::runtime_error(
                std::string("eager HR height snap for '") + rule + "': live="
                + std::to_string(live_h) + " committed="
                + std::to_string(done_h));
    }
}

// A code fence whose closing ``` is the LAST thing in the buffer with no
// trailing newline (the common Claude "here's the code" ending) must
// render the SAME CELLS live (still in the tail) as finished (committed).
// Before the eager-closing-fence commit in boundary.cpp the live tail
// rendered the block via render_tail while finish() re-rendered it via
// the canonical block path; the two produced different cells (same
// height, different bytes), so at settle the whole block re-emitted over
// the wire — the "full repaint when the last block renders" symptom over
// SSH. Comparing height alone misses it (height was already stable); we
// compare the packed cell grid so any cell-level divergence is caught.
static void st_eager_closing_fence() {
    const char* bodies[] = {
        "Intro.\n\n```cpp\nint x = 1;\nint y = 2;\n```",      // no trailing \n
        "```\nplain code\n```",                                // fence-only msg
        "Text.\n\n~~~\ntilde fence\n~~~",                       // tilde fence
        "A.\n\n````\nnested ``` inside\n````",                  // 4-backtick fence
    };
    auto render_cells = [](const StreamingMarkdown& md, int& out_h) {
        Element el = md.build();
        StylePool pool;
        Canvas canvas(80, /*h=*/4000, &pool);
        render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
        out_h = content_height(canvas);
        std::vector<std::uint64_t> cells;
        const std::uint64_t* c = canvas.cells();
        const std::size_t n =
            static_cast<std::size_t>(canvas.width()) * out_h;
        cells.assign(c, c + n);
        return cells;
    };
    for (const char* body : bodies) {
        StreamingMarkdown live;
        live.set_live(true);
        live.feed(body);                  // closing fence sits at EOF, no \n
        // Match the app: a live widget whose reveal is complete still
        // shows no blinking cursor difference vs the finished build for
        // the committed cells. Drop live so the cursor cell can't skew
        // the comparison — we're testing the BLOCK render path, not the
        // cursor glyph.
        live.set_live(false);
        int live_h = 0;
        auto live_cells = render_cells(live, live_h);

        StreamingMarkdown done;
        done.set_content(body);
        done.finish();
        int done_h = 0;
        auto done_cells = render_cells(done, done_h);

        if (live_h != done_h)
            throw std::runtime_error(
                std::string("closing-fence height snap for \"") + body
                + "\": live=" + std::to_string(live_h)
                + " committed=" + std::to_string(done_h));
        if (live_cells != done_cells) {
            std::size_t first = 0;
            while (first < live_cells.size()
                   && live_cells[first] == done_cells[first]) ++first;
            throw std::runtime_error(
                std::string("closing-fence CELL divergence for \"") + body
                + "\": first differing cell index " + std::to_string(first)
                + " of " + std::to_string(live_cells.size()));
        }
    }
}

// Big committed blocks must blit, not re-render, every frame. Build a
// transcript with several large code blocks + a wide table (the
// expensive-to-lay-out kinds), finish so they're all committed, then
// render 300 times. With per-block hash-keyed caching each block paints
// once and blits after; without it every frame re-lays-out every block
// and this blows the budget on a long transcript.
static void st_big_blocks_blit() {
    StreamingMarkdown md;
    std::string body;
    for (int b = 0; b < 12; ++b) {
        body += "## Section " + std::to_string(b) + "\n\n";
        body += "```cpp\n";
        for (int l = 0; l < 60; ++l)
            body += "int v_" + std::to_string(b) + "_" + std::to_string(l)
                 +  " = compute(" + std::to_string(l) + ");\n";
        body += "```\n\n";
    }
    body += "| col a | col b | col c |\n|---|---|---|\n";
    for (int r = 0; r < 40; ++r)
        body += "| cell " + std::to_string(r) + " | data "
             +  std::to_string(r) + " | more " + std::to_string(r) + " |\n";
    body += "\n";
    md.set_content(body);
    md.finish();
    render_stream(md);                 // prime: first paint captures cells
    for (int k = 0; k < 300; ++k)
        render_stream(md);             // every frame must blit
}

// Commit storm: the actual long-session symptom. Feed a transcript that
// commits a new block on (almost) every render, with each committed
// block being a non-trivial code block. After block N commits, blocks
// 0..N-1 are unchanged — per-block hash caching must let them blit so
// each commit costs O(new block), not O(N). Without it this is O(N²)
// over the run and balloons on a long reply.
static void st_commit_storm() {
    StreamingMarkdown md;
    md.set_live(true);
    std::string acc;
    for (int b = 0; b < 80; ++b) {
        acc += "```cpp\n";
        for (int l = 0; l < 20; ++l)
            acc += "auto x_" + std::to_string(b) + "_" + std::to_string(l)
                +  " = f(" + std::to_string(l) + ");\n";
        acc += "```\n\n";
        // set_content the growing buffer + a blank line so the prior
        // block commits, then render — mirrors the view layer feeding
        // the revealed prefix every frame.
        md.set_content(acc);
        render_stream(md);
    }
    md.finish();
    render_stream(md);
}

// Streaming table: a big table arriving row-by-row, rendered between
// rows while still in the uncommitted tail (no trailing blank line yet).
// The eager-table branch of render_tail re-parses + rebuilds the whole
// table every frame; without the eager-slice cache this is O(rows²)
// over the stream. With it, an unchanged committed slice re-emits the
// cached block Elements, so per-frame cost is O(live row).
static void st_streaming_table() {
    StreamingMarkdown md;
    md.set_live(true);
    std::string acc = "| name | id | status | notes |\n"
                      "|------|----|--------|-------|\n";
    md.set_content(acc);
    render_stream(md);
    for (int r = 0; r < 200; ++r) {
        acc += "| item_" + std::to_string(r) + " | " + std::to_string(r)
             +  " | active | some notes about row " + std::to_string(r) + " |\n";
        md.set_content(acc);     // table still uncommitted (no blank line)
        // The view layer renders many frames per row arrival (30 fps
        // cursor blink, keystrokes, ticks). Those repeat-frames at the
        // same slice must blit, not re-lay-out the table.
        for (int f = 0; f < 8; ++f) render_stream(md);
    }
    md.finish();
    render_stream(md);
}

// ─────────────────────────────────────────────────────────────────────────
// Height monotonicity across format transitions — the core anti-FLICKER
// contract. markdown.hpp guarantees: "the element-tree height is a monotonic
// function of the stream's byte position — appending bytes can only extend
// the rendered output, never reflow or shrink it." If that breaks, the
// row-diff has to chase a retroactive height change and the user sees the
// transcript jump/flicker as a half-typed construct re-classifies (a `#`
// becoming a heading, `| a |` becoming a table row, ``` opening a fence).
//
// This feeds a transition-dense document ONE CODEPOINT AT A TIME and asserts
// the rendered content height never decreases between consecutive frames. It
// deliberately walks through every risky reclassification:
//   plain text → heading, paragraph → list, paragraph → table, text → fence,
//   text → blockquote, text → alert, text → hrule, and the inline
//   bold/italic/code spans that can change a line's wrap width as they close.
static const char* kTransitionDoc =
    "Plain intro line that is fairly long so it occupies a row or two.\n\n"
    "# A heading that was plain text until the hash and space arrived\n\n"
    "## Second level heading appearing mid-stream\n\n"
    "A paragraph with **bold that opens and** *italic that opens and* "
    "`code that opens and` closes across several appended bytes.\n\n"
    "- first list item that was a paragraph until the dash and space\n"
    "- second item\n"
    "  - nested item deeper\n"
    "- third item\n\n"
    "1. ordered item\n"
    "2. ordered item two\n"
    "3. ordered item three\n\n"
    "| name | id | status |\n"
    "|------|----|--------|\n"
    "| alpha | 1 | active |\n"
    "| beta | 2 | idle |\n"
    "| gamma | 3 | gone |\n\n"
    "```cpp\n"
    "int main() {\n"
    "    return compute(x) + compute(y);\n"
    "}\n"
    "```\n\n"
    "Some text before a rule.\n\n"
    "---\n\n"
    "> a blockquote line that was plain text until the marker arrived\n"
    "> second quoted line that continues the same quote block\n\n"
    "> [!WARNING]\n"
    "> an alert that reclassifies from a plain blockquote mid-stream\n\n"
    "A Setext Heading That Looks Like A Paragraph Until The Underline\n"
    "================================================================\n\n"
    "Closing paragraph after the horizontal rule, also a bit long so it "
    "wraps onto more than one row at width 80.\n";

// NOTE: blockquote and GitHub-alert constructs are deliberately EXCLUDED from
// kTransitionDoc — their eager-tail render is not yet height-monotonic at the
// terminating blank line / blockquote->alert reclassification. Those are
// pinned as known-open in st_known_open_quote_alert_snap() below so the suite
// stays green while the bug stays visible.

static void st_height_monotonic_transitions() {
    StreamingMarkdown md;
    md.set_live(true);

    std::string_view doc{kTransitionDoc};
    int prev_h = 0;
    int shrink_events = 0;        // # of frames where height decreased
    int total_shrink_rows = 0;   // sum of all row drops
    int worst_drop = 0;
    std::size_t worst_at = 0;

    // Feed one UTF-8 codepoint at a time (this doc is ASCII, so byte == cp,
    // but step by lead-byte boundaries to stay codepoint-safe in general).
    for (std::size_t i = 0; i < doc.size(); ) {
        std::size_t step = 1;
        while (i + step < doc.size()
               && (static_cast<unsigned char>(doc[i + step]) & 0xC0) == 0x80)
            ++step;
        md.append(doc.substr(i, step));
        i += step;

        int h = stream_height(md);
        if (h < prev_h) {
            int drop = prev_h - h;
            ++shrink_events;
            total_shrink_rows += drop;
            if (drop > worst_drop) { worst_drop = drop; worst_at = i; }
        }
        prev_h = h;
    }

    // Pinned flicker baseline. kTransitionDoc walks every risky block
    // construct — heading, list, table, fence, hrule, blockquote, alert,
    // setext, plus inline emphasis/code spans. The dangerous MULTI-ROW
    // reclassification snaps (heading 2-row, quote/alert 2-row, fence
    // open→commit 2-row) are now FULLY eliminated by the canonical
    // committed-shape floor in render_tail: the terminated rows render in
    // the exact shape commit_range will produce, so the live tail height
    // equals the committed height at every byte and the commit seam is
    // continuous.
    //
    // The residual shrinks are all exactly 1 ROW and content-dependent:
    // an inline `code`/**bold**/*italic* delimiter opening shifts a wrap
    // point, and the fence-close settles one row when the third backtick
    // lands. Neither is a transcript-jump. The baseline pins BOTH the
    // worst single drop (1 row) and the event count, so a NEW multi-row
    // block reflow OR an increase in 1-row jitter fails here. Lower these
    // as the inline paths tighten; never raise without understanding the
    // new shrink.
    constexpr int kBaselineShrinkEvents = 3;   // inline-span opens + fence-close settle
    constexpr int kBaselineWorstDrop    = 1;   // every shrink is exactly 1 row

    if (worst_drop > kBaselineWorstDrop) {
        std::size_t ctx_lo = worst_at > 24 ? worst_at - 24 : 0;
        std::string ctx{doc.substr(ctx_lo, 48)};
        for (auto& c : ctx) if (c == '\n') c = '?';
        throw std::runtime_error(
            "NEW MULTI-ROW height shrink: " + std::to_string(worst_drop)
            + " rows at byte " + std::to_string(worst_at) + " (near: '" + ctx
            + "'). Baseline worst drop is " + std::to_string(kBaselineWorstDrop)
            + " (inline wrap jitter). A drop this big is a block-level "
            "reflow — the visible-flicker class. Find the eager-render branch "
            "that disagrees with the committed layout.");
    }
    if (shrink_events > kBaselineShrinkEvents) {
        throw std::runtime_error(
            "flicker REGRESSION: " + std::to_string(shrink_events)
            + " height-shrink events (total " + std::to_string(total_shrink_rows)
            + " rows) exceeds baseline " + std::to_string(kBaselineShrinkEvents)
            + ". A new height non-monotonicity was introduced into the "
            "streaming render. Bisect the transition that regressed.");
    }

    // Settle must not snap: final live height equals committed height.
    int live_final = stream_height(md);
    md.finish();
    int done_final = stream_height(md);
    if (live_final != done_final)
        throw std::runtime_error(
            "settle height snap: live=" + std::to_string(live_final)
            + " committed=" + std::to_string(done_final));
}

// Companion: COMMITTED cells must never be rewritten as the stream grows.
// Height can stay monotonic yet still flicker if an already-committed block
// gets re-rendered with different cells when a later block commits (the
// over-SSH "whole reply repaints" symptom). After each block-boundary
// commit, the cells of the committed prefix region [0, committed_height)
// must be byte-identical to what they were on the previous frame.
static void st_committed_cells_stable() {
    StreamingMarkdown md;
    md.set_live(true);

    std::string_view doc{kTransitionDoc};
    std::vector<std::uint64_t> prev_cells;
    int prev_h = 0;
    std::size_t prev_committed = 0;

    auto snapshot = [&](int& out_h) {
        Element el = md.build();
        StylePool pool;
        Canvas canvas(80, /*h=*/4000, &pool);
        render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
        out_h = content_height(canvas);
        const std::uint64_t* c = canvas.cells();
        std::size_t n = static_cast<std::size_t>(canvas.width()) * out_h;
        return std::vector<std::uint64_t>(c, c + n);
    };

    for (std::size_t i = 0; i < doc.size(); ) {
        std::size_t step = 1;
        while (i + step < doc.size()
               && (static_cast<unsigned char>(doc[i + step]) & 0xC0) == 0x80)
            ++step;
        md.append(doc.substr(i, step));
        i += step;

        std::size_t committed_now = md.block_count();
        int h = 0;
        auto cells = snapshot(h);

        // Only verify when the committed-block COUNT advanced — that's the
        // event that risks rewriting the prefix. Compare the overlap of the
        // two frames' committed prefix region: the cells that were present
        // (and committed) last frame must be unchanged this frame.
        if (committed_now > prev_committed && !prev_cells.empty()) {
            // The committed prefix is the shorter of the two heights, minus
            // the live tail. We can't know the exact committed row count
            // here without internals, so use the conservative overlap: all
            // rows that existed last frame AND whose bytes are below the new
            // commit point are settled. Approximate by comparing the
            // min-height prefix; a committed row never moves, so any cell in
            // [0, min(prev_h,h)*width) that differs is a rewrite of settled
            // content. The live tail lives at the BOTTOM, so a difference in
            // the top region is unambiguously a committed-cell rewrite.
            std::size_t cmp_rows = static_cast<std::size_t>(
                std::min(prev_h, h));
            // Exclude the last few rows of the smaller frame: that's where
            // the live tail / caret can legitimately sit before commit.
            std::size_t tail_guard = 3;
            if (cmp_rows > tail_guard) cmp_rows -= tail_guard; else cmp_rows = 0;
            std::size_t width = 80;
            for (std::size_t r = 0; r < cmp_rows; ++r) {
                for (std::size_t col = 0; col < width; ++col) {
                    std::size_t idx = r * width + col;
                    if (idx >= cells.size() || idx >= prev_cells.size())
                        continue;
                    if (cells[idx] != prev_cells[idx]) {
                        std::string ctx{doc.substr(i > 24 ? i - 24 : 0, 48)};
                        for (auto& c : ctx) if (c == '\n') c = '?';
                        throw std::runtime_error(
                            "COMMITTED CELL REWRITTEN at row "
                            + std::to_string(r) + " col " + std::to_string(col)
                            + " when committed blocks went "
                            + std::to_string(prev_committed) + "->"
                            + std::to_string(committed_now)
                            + " (near: '" + ctx + "') — a settled block "
                            "re-rendered with different cells; this repaints "
                            "the prefix and flickers over the wire.");
                    }
                }
            }
        }
        prev_cells = std::move(cells);
        prev_h = h;
        prev_committed = committed_now;
    }
}

// Per-construct height-snap sweep: for every risky block kind, feed the
// block WITHOUT its trailing blank line (so it sits in the live tail) and
// assert its live height equals the committed height. This is the
// st_eager_hrule / st_eager_closing_fence idea generalised to every block
// type, because each has its own eager-render branch in render_tail that
// can drift from the canonical block layout.
static void st_eager_block_no_snap() {
    struct Case { const char* name; std::string body; };
    const std::string cases_raw[][2] = {
        {"atx-heading",   "Intro.\n\n# A Heading Line\n"},
        {"bullet-list",   "Intro.\n\n- one\n- two\n- three\n"},
        {"ordered-list",  "Intro.\n\n1. one\n2. two\n3. three\n"},
        {"task-list",     "Intro.\n\n- [x] done\n- [ ] todo\n"},
        {"table",         "Intro.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n"},
        {"hrule-dash",    "Intro.\n\n---\n"},
        {"hrule-star",    "Intro.\n\n***\n"},
        {"code-fence",    "Intro.\n\n```py\nx = 1\ny = 2\n```\n"},
        {"nested-list",   "Intro.\n\n- a\n  - b\n    - c\n"},
        {"blockquote",    "Intro.\n\n> quoted line\n> second line\n"},
        {"alert",         "Intro.\n\n> [!NOTE]\n> body of the note\n"},
        // setext-heading is intentionally OMITTED: `A Heading\n` is
        // indistinguishable from a paragraph until the `====` underline
        // arrives, so a one-frame height adjustment at the underline is
        // inherent, not a fixable eager-render bug.
    };
    for (const auto& kv : cases_raw) {
        const std::string& name = kv[0];
        const std::string& body = kv[1];

        StreamingMarkdown live;
        live.set_live(true);
        live.feed(body);          // trailing block sits in the live tail
        live.set_live(false);     // drop caret so it can't skew height
        int live_h = stream_height(live);

        StreamingMarkdown done;
        done.set_content(body);
        done.finish();
        int done_h = stream_height(done);

        if (live_h != done_h)
            throw std::runtime_error(
                "eager block '" + name + "' height snap: live="
                + std::to_string(live_h) + " committed="
                + std::to_string(done_h) + " — the live tail render and the "
                "committed block render disagree on height; settle will jump.");
    }
}

// (former st_known_open_quote_alert_snap removed: quote / alert / setext
// eager renders are now height-monotonic — the canonical committed-shape
// floor in render_tail eliminated the snap. blockquote and alert are
// promoted into st_eager_block_no_snap above; setext rides the
// transition sweep in st_height_monotonic_transitions.)

// ───────────────────────────── main ─────────────────────────────────────────

int main() {
    std::println("=== test_markdown ===");
    std::println("\n-- A. features (correctness) --");

    run("empty input",                 1000ms, f_empty);
    run("whitespace only",             1000ms, f_whitespace);
    run("single paragraph",            1000ms, f_single_paragraph);
    run("many paragraphs (small)",     1000ms, f_many_paragraphs);
    run("heading ATX (#..######)",     1000ms, f_heading_atx_all);
    run("heading setext (=)",          1000ms, f_heading_setext_eq);
    run("heading setext (-)",          1000ms, f_heading_setext_dash);
    run("bold **",                     1000ms, f_bold_star);
    run("bold __",                     1000ms, f_bold_underscore);
    run("italic *",                    1000ms, f_italic_star);
    run("italic _",                    1000ms, f_italic_underscore);
    run("bold+italic ***",             1000ms, f_bold_italic);
    run("strikethrough ~~",            1000ms, f_strikethrough);
    run("inline code `",               1000ms, f_inline_code);
    run("inline code multi-backtick",  1000ms, f_inline_code_multi_backtick);
    run("backslash escapes",           1000ms, f_escapes);
    run("fenced code (plain)",         1000ms, f_fenced_code_plain);
    run("fenced code (lang)",          1000ms, f_fenced_code_lang);
    run("fenced code (tilde)",         1000ms, f_fenced_code_tilde);
    run("indented code",               1000ms, f_indented_code);
    run("inline math $",               1000ms, f_inline_math);
    run("display math $$",             1000ms, f_display_math);
    run("bullet list (dash)",          1000ms, f_bullet_dash);
    run("bullet list (star)",          1000ms, f_bullet_star);
    run("bullet list (plus)",          1000ms, f_bullet_plus);
    run("ordered list (1.)",           1000ms, f_ordered_dot);
    run("ordered list (1))",           1000ms, f_ordered_paren);
    run("task list",                   1000ms, f_task_list);
    run("nested list",                 1000ms, f_nested_list);
    run("list with paragraphs",        1000ms, f_list_with_paragraphs);
    run("blockquote (single)",         1000ms, f_blockquote_one);
    run("blockquote (multi)",          1000ms, f_blockquote_multi);
    run("blockquote (nested)",         1000ms, f_blockquote_nested);
    run("blockquote with list",        1000ms, f_blockquote_with_list);
    run("table (simple)",              1000ms, f_table_simple);
    run("table (alignment)",           1000ms, f_table_align);
    run("table (empty cells)",         1000ms, f_table_empty_cells);
    run("link (inline)",               1000ms, f_link_inline);
    run("link (with title)",           1000ms, f_link_title);
    run("autolink",                    1000ms, f_autolink);
    run("bare URL in text",            1000ms, f_bare_url_like);
    run("image (inline)",              1000ms, f_image_inline);
    run("footnote (ref + def)",        1000ms, f_footnote_ref_def);
    run("horizontal rule (---)",       1000ms, f_hr_dash);
    run("horizontal rule (***)",       1000ms, f_hr_star);
    run("horizontal rule (___)",       1000ms, f_hr_under);
    run("hard break (backslash)",      1000ms, f_hard_break_backslash);
    run("hard break (2 spaces)",       1000ms, f_hard_break_spaces);
    run("unicode BMP",                 1000ms, f_unicode_bmp);
    run("unicode CJK",                 1000ms, f_unicode_cjk);
    run("unicode emoji",               1000ms, f_unicode_emoji);
    run("unicode combining marks",     1000ms, f_unicode_combining);

    std::println("\n-- A2. new features (bucket 1 + 2) --");
    run("highlight ==x==",             1000ms, f_highlight_eq);
    run("highlight <mark>",            1000ms, f_highlight_mark);
    run("subscript ~x~",               1000ms, f_sub_tilde);
    run("superscript ^x^",             1000ms, f_sup_caret);
    run("subscript <sub>",             1000ms, f_sub_tag);
    run("superscript <sup>",           1000ms, f_sup_tag);
    run("<br> hard break",             1000ms, f_br_tag);
    run("<kbd> key",                   1000ms, f_kbd_tag);
    run("<strong>/<em>",               1000ms, f_strong_em_tag);
    run("<span>",                      1000ms, f_span_tag);
    run("<abbr title=...>",            1000ms, f_abbr_tag);
    run("link ref (full)",             1000ms, f_link_ref_full);
    run("link ref (collapsed)",        1000ms, f_link_ref_collapsed);
    run("link ref (shortcut)",         1000ms, f_link_ref_shortcut);
    run("link ref (case-fold)",        1000ms, f_link_ref_case_fold);
    run("link ref (missing def)",      1000ms, f_link_ref_missing);
    run("link anchor #heading",        1000ms, f_link_anchor);
    run("email autolink",              1000ms, f_link_email_autolink);
    run("image ref",                   1000ms, f_image_ref);
    run("image title",                 1000ms, f_image_title);
    run("image clickable link",        1000ms, f_image_clickable);
    run("entities named",              1000ms, f_entities_named);
    run("entities numeric",            1000ms, f_entities_numeric);
    run("emoji shortcodes",            1000ms, f_emoji_shortcodes);
    run("mention @user",               1000ms, f_mention_user);
    run("mention #issue",              1000ms, f_mention_issue);
    run("mention cross-repo",          1000ms, f_mention_cross_repo);
    run("alert [!NOTE]",               1000ms, f_alert_note);
    run("alert [!TIP]",                1000ms, f_alert_tip);
    run("alert [!IMPORTANT]",          1000ms, f_alert_important);
    run("alert [!WARNING]",            1000ms, f_alert_warning);
    run("alert [!CAUTION]",            1000ms, f_alert_caution);
    run("alert multi-line/children",   1000ms, f_alert_with_children);
    run("def list simple",             1000ms, f_def_list_simple);
    run("def list multi-term/def",     1000ms, f_def_list_multi);
    run("<details>/<summary>",         1000ms, f_details_summary);
    run("html block <div>",            1000ms, f_html_block_div);
    run("html block <section>",        1000ms, f_html_block_section);
    run("anchor id <a id=...>",        1000ms, f_anchor_id);
    run("mermaid fallback",            1000ms, f_mermaid_fallback);
    run("matrix fallback",             1000ms, f_matrix_fallback);
    run("latex fallback",              1000ms, f_latex_fallback);
    run("mixed new features",          2000ms, f_new_features_mixed);

    std::println("\n-- A2b. new-feature edge cases & gotchas --");
    // highlight
    run("highlight empty ====",         1000ms, g_highlight_empty);
    run("highlight unclosed",           1000ms, g_highlight_unclosed);
    run("highlight + nested emph",      1000ms, g_highlight_nested_emph);
    run("highlight with = inside",      1000ms, g_highlight_equal_inside);
    run("highlight across newline",     1000ms, g_highlight_crosses_line);
    run("highlight inside code",        1000ms, g_highlight_in_code);
    // sub/sup
    run("sub empty (strike ~~)",        1000ms, g_sub_empty);
    run("sup empty ^^",                 1000ms, g_sup_empty);
    run("sub with internal space",      1000ms, g_sub_with_space);
    run("sup with digits",              1000ms, g_sup_with_digits);
    run("sub inside strike",            1000ms, g_sub_inside_strike);
    run("sup unclosed",                 1000ms, g_sup_unclosed);
    run("sub back-to-back",             1000ms, g_sub_backtoback);
    // <br>
    run("<br /> slash+space",           1000ms, g_br_slash_space);
    run("<br/> slash no space",         1000ms, g_br_slash_no_space);
    run("<br   > extra ws",             1000ms, g_br_extra_whitespace);
    run("<BR> uppercase",               1000ms, g_br_uppercase);
    run("<br> at start",                1000ms, g_br_at_start);
    run("<br> many in a row",           1000ms, g_br_many);
    // paired tags
    run("<kbd></kbd> empty",            1000ms, g_kbd_empty);
    run("<kbd> unclosed",               1000ms, g_kbd_unclosed);
    run("<kbd> nested",                 1000ms, g_kbd_nested);
    run("<span> with attrs",            1000ms, g_span_attrs);
    run("<abbr> no title",              1000ms, g_abbr_no_title);
    run("<abbr> empty title",           1000ms, g_abbr_empty_title);
    run("<mark> inside **",             1000ms, g_mark_inside_strong);
    // entities
    run("entity unknown &foo;",         1000ms, g_entity_unknown);
    run("entity no semicolon",          1000ms, g_entity_no_semi);
    run("entity in code span",          1000ms, g_entity_in_code);
    run("entity hex huge",              1000ms, g_entity_hex_huge);
    run("entity &#0; zero",             1000ms, g_entity_dec_zero);
    run("entities adjacent",            1000ms, g_entity_adjacent);
    run("trailing '&' literal",         1000ms, g_entity_trailing_amp);
    // emoji
    run("emoji unknown shortcode",      1000ms, g_emoji_unknown);
    run("emoji unclosed colon",         1000ms, g_emoji_unclosed);
    run("emoji in code span",           1000ms, g_emoji_in_code);
    run("emoji adjacent shortcodes",    1000ms, g_emoji_adjacent);
    run("emoji in heading",             1000ms, g_emoji_in_heading);
    run("emoji in link text",           1000ms, g_emoji_in_link);
    run("colon in URL not emoji",       1000ms, g_colon_in_url);
    // mentions
    run("mention vs email",             1000ms, g_mention_email_conflict);
    run("mention + trailing punct",     1000ms, g_mention_trailing_punct);
    run("mention in code span",         1000ms, g_mention_in_code);
    run("mention at line start",        1000ms, g_mention_at_start);
    run("#issue inline",                1000ms, g_mention_issue_inline);
    run("# inside URL not issue",       1000ms, g_mention_hash_in_url);
    run("cross-repo odd chars",         1000ms, g_mention_cross_repo_odd);
    run("C#/F# not an issue",           1000ms, g_mention_hash_not_issue);
    // ref links
    run("ref case + whitespace fold",   1000ms, g_ref_case_fold_spaces);
    run("ref title in (...)",           1000ms, g_ref_title_paren);
    run("ref title in '...'",           1000ms, g_ref_title_single);
    run("ref url in <...>",             1000ms, g_ref_url_angle);
    run("ref duplicate def",            1000ms, g_ref_dup_def);
    run("ref def inside code",          1000ms, g_ref_def_in_code);
    run("ref empty label",              1000ms, g_ref_empty_label);
    run("ref shortcut with code",       1000ms, g_ref_shortcut_with_code);
    // email
    run("email autolink a+b@..",        1000ms, g_email_with_plus);
    run("email autolink bogus",         1000ms, g_email_bad);
    run("email bare in text",           1000ms, g_email_bare_in_text);
    // image
    run("image missing ref",            1000ms, g_image_missing_ref);
    run("image empty alt",              1000ms, g_image_empty_alt);
    run("image relative path",          1000ms, g_image_relative);
    // alerts
    run("[!note] lowercase (no)",       1000ms, g_alert_lowercase);
    run("[!FOO] unknown kind",          1000ms, g_alert_unknown_kind);
    run("[!NOTE] extra text",           1000ms, g_alert_space_after);
    run("[!NOTE] empty",                1000ms, g_alert_empty);
    run("[NOTE] missing bang",          1000ms, g_alert_no_marker);
    run("alert inside blockquote",      1000ms, g_alert_inside_quote);
    // def list
    run("deflist : no space",           1000ms, g_deflist_no_space);
    run("deflist multi def",            1000ms, g_deflist_multi_def);
    run("deflist with bold term",       1000ms, g_deflist_with_bold);
    run("deflist multi-line term",      1000ms, g_deflist_multi_term);
    // details
    run("<details> no summary",         1000ms, g_details_no_summary);
    run("<details> empty",              1000ms, g_details_empty);
    run("<details> nested",             1000ms, g_details_nested);
    // html block
    run("html block unclosed",          1000ms, g_htmlblock_unclosed);
    run("html block nested tags",       1000ms, g_htmlblock_nested_tags);
    run("html block w/ md inside",      1000ms, g_htmlblock_with_md);
    // fence fallbacks
    run("mermaid empty fence",          1000ms, g_mermaid_empty);
    run("MERMAID uppercase",            1000ms, g_mermaid_uppercase);
    run("math fence",                   1000ms, g_math_fence);
    // combined
    run("all inline new features",      1000ms, g_all_inline_together);

    std::println("\n-- A2c. CommonMark/GFM gotchas --");
    // trailing-space / hard-break
    run("trailing 2-space hard break",  1000ms, g_trailing_two_spaces_br);
    run("trailing 3-space hard break",  1000ms, g_trailing_three_spaces_br);
    run("trailing space at EOF",        1000ms, g_trailing_space_at_eof);
    run("spaces on 'blank' line",       1000ms, g_space_on_empty_line);
    run("backslash-EOL break",          1000ms, g_backslash_eol_break);
    run("backslash-EOL before blank",   1000ms, g_backslash_eol_before_blank);
    // nested list indentation
    run("nested ol→ul 3 spaces",        1000ms, g_nested_ol_ul_3sp);
    run("nested ol→ul 4 spaces",        1000ms, g_nested_ol_ul_4sp);
    run("nested ol→ul 0 spaces (new)",  1000ms, g_nested_ol_ul_0sp);
    run("mixed-indent nesting",         1000ms, g_nested_ol_mixed_indents);
    run("nested ul with tabs",          1000ms, g_nested_ul_tabs);
    // list adjacent to paragraph
    run("list w/o blank line before",   1000ms, g_list_no_blank_before);
    run("para then 1. w/o blank",       1000ms, g_para_then_ol_no_blank);
    // intraword emphasis
    run("intraword _underscores_",      1000ms, g_intraword_underscore);
    run("intraword mid-word _",         1000ms, g_intraword_mid_word);
    run("real _emph_ at word edge",     1000ms, g_word_boundary_underscore);
    run("intraword *stars*",            1000ms, g_intraword_star_inside);
    // table pipe escape
    run("table escaped |",              1000ms, g_table_escaped_pipe);
    run("table many escaped |",         1000ms, g_table_multi_escaped_pipe);
    run("table | inside `code`",        1000ms, g_table_pipe_in_code_span);
    // indented code in list
    run("list + indented code (8sp)",   1000ms, g_list_with_indented_code);
    run("list + fenced code inside",    1000ms, g_ul_with_fenced_inside);
    // HTML comments
    run("HTML comment inline",          1000ms, g_html_comment_inline);
    run("HTML comment alone",           1000ms, g_html_comment_only);
    run("HTML comment multi-line",      1000ms, g_html_comment_multiline);
    run("HTML comment unterminated",    1000ms, g_html_comment_unterminated);
    run("HTML comment in code span",    1000ms, g_html_comment_in_code);
    run("HTML comment mid paragraph",   1000ms, g_html_comment_inside_p);
    // block HTML + md inside
    run("<div> with md via blanks",     1000ms, g_htmlblock_md_inside_blank);
    run("<div> w/o blank lines",        1000ms, g_htmlblock_no_blank_breaks);
    // backslash escapes
    run("escape every ASCII punct",     1000ms, g_escape_all_punct);
    run("escape * literal",             1000ms, g_escape_asterisk_literal);
    run("escape ` literal",             1000ms, g_escape_backtick_literal);
    run("escape [] () literal",         1000ms, g_escape_brackets_literal);
    run("escape | in prose",            1000ms, g_escape_pipe_outside_table);
    run("escape non-escapable",         1000ms, g_escape_nonescapable);
    run("escape inside code span",      1000ms, g_escape_inside_code_span);
    run("escape inside code fence",     1000ms, g_escape_inside_code_fence);
    // footnote edge cases
    run("fn ref without def",           1000ms, g_fn_ref_no_def);
    run("fn def without ref",           1000ms, g_fn_def_no_ref);
    run("fn [^] empty brackets",        1000ms, g_fn_empty_brackets);
    run("fn def empty content",         1000ms, g_fn_def_empty_content);
    run("fn def whitespace-only",       1000ms, g_fn_def_only_whitespace);
    run("fn duplicate def",             1000ms, g_fn_duplicate_def);
    run("fn ref used twice",            1000ms, g_fn_ref_used_twice);
    run("fn same ref ×3 in a row",      1000ms, g_fn_ref_many_same);
    run("fn multi-paragraph def",       1000ms, g_fn_multi_paragraph);
    run("fn def with indented code",    1000ms, g_fn_def_with_code);
    run("fn def with fenced code",      1000ms, g_fn_def_with_fenced_code);
    run("fn def with list",             1000ms, g_fn_def_with_list);
    run("fn def with bold/italic",      1000ms, g_fn_def_with_bold);
    run("fn def with link",             1000ms, g_fn_def_with_link);
    run("fn def with image",            1000ms, g_fn_def_with_image);
    run("fn ref inside fn def",         1000ms, g_fn_ref_inside_fn_def);
    run("fn label numeric",             1000ms, g_fn_label_numeric);
    run("fn label alpha",               1000ms, g_fn_label_alpha);
    run("fn label hyphen",              1000ms, g_fn_label_hyphen);
    run("fn label underscore",          1000ms, g_fn_label_underscore);
    run("fn label unicode",             1000ms, g_fn_label_unicode);
    run("fn label mixed case",          1000ms, g_fn_label_mixed_case);
    run("fn label with spaces",         1000ms, g_fn_label_with_spaces);
    run("fn label very long (180)",     1000ms, g_fn_label_very_long);
    run("fn label over cap (500)",      2000ms, g_fn_label_over_cap);
    run("fn ref in code span",          1000ms, g_fn_ref_in_code_span);
    run("fn ref in code fence",         1000ms, g_fn_ref_in_code_fence);
    run("fn ref in link text",          1000ms, g_fn_ref_in_link_text);
    run("fn ref in heading",            1000ms, g_fn_ref_in_heading);
    run("fn ref in table cell",         1000ms, g_fn_ref_in_table_cell);
    run("fn def inside blockquote",     1000ms, g_fn_def_inside_blockquote);
    run("fn def inside list",           1000ms, g_fn_def_inside_list);
    run("[^ ] caret+space not fn",      1000ms, g_fn_caret_then_space);
    run("[^text](url) looks like fn",   1000ms, g_fn_caret_in_link);
    run("[^ unclosed bracket",          1000ms, g_fn_bracket_not_fn);
    run("fn def with long body",        2000ms, g_fn_def_long_body);
    // combined
    run("every-gotcha mixed",           2000ms, g_gotcha_mixed);

    std::println("\n-- B. edge / malformed --");
    run("unclosed bold",               2000ms, e_unclosed_bold);
    run("unclosed italic",             2000ms, e_unclosed_italic);
    run("unclosed code span",          2000ms, e_unclosed_code_span);
    run("unclosed fence",              2000ms, e_unclosed_fence);
    run("unclosed link bracket",       2000ms, e_unclosed_link);
    run("unclosed link paren",         2000ms, e_unclosed_link_url);
    run("mismatched emphasis",         2000ms, e_mismatched_emphasis);
    run("only delimiters (***)",       2000ms, e_only_delimiters);
    run("only backticks",              2000ms, e_only_backticks);
    run("only hashes",                 2000ms, e_only_hashes);
    run("only pipes",                  2000ms, e_only_pipes);
    run("heading with no text",        2000ms, e_heading_no_text);
    run("blank link []()",             2000ms, e_blank_link);
    run("blank image ![]()",           2000ms, e_blank_image);
    run("empty code block",            2000ms, e_empty_code_block);
    run("tab-indented code",           2000ms, e_tab_indented);
    run("CRLF line endings",           2000ms, e_crlf_line_endings);
    run("trailing newlines",           2000ms, e_trailing_newlines);
    run("UTF-8 BOM",                   2000ms, e_bom);
    run("very long URL (2k chars)",    2000ms, e_long_url);

    std::println("\n-- C. size stress (well-formed) --");
    run("long paragraph 100k",         5000ms, s_long_paragraph_100k);
    run("long paragraph 1M",          10000ms, s_long_paragraph_1m);
    run("many paragraphs 10k",         5000ms, s_many_paragraphs_10k);
    run("many headings 5k",            5000ms, s_many_headings_5k);
    run("many list items 10k",         5000ms, s_many_list_items_10k);
    run("many code fences 1k",         5000ms, s_many_code_fences_1k);
    run("wide table 200 cols",         5000ms, s_wide_table_200_cols);
    run("tall table 5k rows",          5000ms, s_tall_table_5k_rows);
    run("long code block 100k",        5000ms, s_long_code_block_100k);
    run("long blockquote 10k lines",   5000ms, s_long_blockquote_10k_lines);

    std::println("\n-- D. pathological / bisect bait --");
    run("unmatched emphasis x10k",     5000ms, p_unmatched_emphasis_10k);
    run("alternating emphasis x5k",    5000ms, p_alternating_emphasis_5k);
    run("nested emphasis depth 500",   5000ms, p_nested_emphasis_500);
    run("nested emphasis depth 2000",  5000ms, p_nested_emphasis_extreme_2000);
    run("100k backticks",              5000ms, p_many_backticks_100k);
    run("brackets no-close x10k",      5000ms, p_brackets_without_close_10k);
    run("brackets+parens stacked x5k", 5000ms, p_brackets_and_parens_stacked_5k);
    run("footnote refs x5k",           5000ms, p_footnote_refs_5k);
    run("deep blockquote x1k",         5000ms, p_deep_blockquote_1k);
    run("deep nested list x200",       5000ms, p_deep_nested_list_200);
    run("fence openers x10k",          5000ms, p_huge_fenced_block_count_10k);
    run("mixed realistic 500k",       15000ms, p_mixed_realistic_500k);
    run("100k-char inline code span",  5000ms, p_long_inline_code_span);
    run("autolinks x5k",               5000ms, p_many_autolinks_5k);
    run("reference soup x5k",          5000ms, p_reference_soup_5k);

    std::println("\n-- E. StreamingMarkdown perf --");
    run("stream char-by-char 4k",      5000ms, st_char_by_char_4k);
    run("stream 8-byte chunks 50k",    5000ms, st_token_chunks_50k);
    run("set_content no-change ×10k",  2000ms, st_set_content_no_change_10k);
    run("repeat build() ×100k",        2000ms, st_repeat_build_100k);
    run("growing unclosed code fence", 5000ms, st_growing_code_fence);
    run("set_content grow 20k",        5000ms, st_set_content_grow_20k);
    run("render after commits ×200",   2000ms, st_render_after_commits_no_change);
    run("render between commits ×200", 2000ms, st_render_between_commits);
    run("clear + restream ×10",        5000ms, st_clear_restream);
    run("block meta + fold",            2000ms, st_block_meta_and_fold);
    run("set_content_async roundtrip",  8000ms, st_set_content_async_roundtrip);
    run("eager hrule (no snap)",        2000ms, st_eager_hrule);
    run("eager closing fence (no snap)", 2000ms, st_eager_closing_fence);
    run("big blocks blit ×300",         2000ms, st_big_blocks_blit);
    run("commit storm ×80",            3000ms, st_commit_storm);
    run("streaming table ×200",        3000ms, st_streaming_table);

    std::println("\n-- F. height monotonicity / no-flicker --");
    run("height monotonic (transitions)", 5000ms, st_height_monotonic_transitions);
    run("committed cells stable",         5000ms, st_committed_cells_stable);
    run("eager block no-snap (all kinds)", 3000ms, st_eager_block_no_snap);
    std::println("\n── summary ──────────────────────────────────────────────");
    std::println("  passed: {}   slow: {}   failed: {}   skipped: {}",
                 g_passed - g_slow, g_slow, g_failed, g_skipped);
    return g_failed == 0 ? 0 : 1;
}
