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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <print>
#include <string>
#include <string_view>

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

    std::println("\n── summary ──────────────────────────────────────────────");
    std::println("  passed: {}   slow: {}   failed: {}   skipped: {}",
                 g_passed - g_slow, g_slow, g_failed, g_skipped);
    return g_failed == 0 ? 0 : 1;
}
