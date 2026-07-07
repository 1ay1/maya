// tests/test_highlight_resume.cpp — the incremental (streaming) resume path of
// md_detail::highlight_code must produce output byte-for-byte identical to a
// full re-tokenise. This is load-bearing: the reveal animation clips rendered
// code by BYTE OFFSET, so any divergence between the streamed prefix render
// and a from-scratch render would desync the glide / corrupt the code fence.
//
// The highlighter carries a thread_local "resume" cache that seeds the
// tokeniser from the last ground-state newline (a newline at which no
// multi-line construct — block comment, triple-quoted string — is open) and
// re-scans only the newly-arrived tail. We stress exactly the constructs that
// carry cross-line lexer state, plus plain code, and assert equivalence.

#include <maya/maya.hpp>
#include <maya/widget/markdown/internal.hpp>

#include <cassert>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"

using namespace maya;

// Extract (content, runs) from the Element the highlighter returns. The
// highlighter always emits a single TextElement.
struct Rendered {
    std::string             content;
    std::vector<StyledRun>  runs;
};

static Rendered render(const std::string& code, const std::string& lang) {
    Element e = md_detail::highlight_code(code, lang);
    const TextElement* t = as_text(e);
    MAYA_TEST_CHECK(t != nullptr, "highlight_code must yield a TextElement");
    return Rendered{t->content, t->runs};
}

static bool runs_equal(const std::vector<StyledRun>& a,
                       const std::vector<StyledRun>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].byte_offset != b[i].byte_offset) return false;
        if (a[i].byte_length != b[i].byte_length) return false;
        if (!(a[i].style == b[i].style)) return false;
    }
    return true;
}

// Stream `code` byte-by-byte through highlight_code (exercising the resume
// cache on every prefix), then assert the FINAL render equals a cold,
// from-scratch render of the same content captured BEFORE streaming.
static void assert_stream_matches_cold(const std::string& code,
                                       const std::string& lang,
                                       std::string_view name) {
    std::println("--- resume: {} ---", name);

    // Cold reference: render the full content once. (This also warms the FNV
    // content cache for `code`, but that only makes the final streamed call a
    // pure hit — still fine, because a hit trivially matches the reference.)
    Rendered cold = render(code, lang);

    // Stream every prefix. Each call after the first is a content-key MISS
    // (the body grows), so it goes through the resume path.
    Rendered last;
    for (std::size_t k = 1; k <= code.size(); ++k) {
        last = render(code.substr(0, k), lang);
    }

    MAYA_TEST_CHECK(last.content == cold.content,
                    "streamed final content must equal cold render");
    MAYA_TEST_CHECK(runs_equal(last.runs, cold.runs),
                    "streamed final runs must equal cold render");
}

int main() {
    // Plain code with keywords, strings, numbers across many lines.
    assert_stream_matches_cold(
        "int main() {\n"
        "    int x = 42;\n"
        "    const char* s = \"hello\";\n"
        "    return x;\n"
        "}\n",
        "cpp", "plain-cpp");

    // Multi-line block comment: the cross-line construct that MUST NOT be
    // frozen while still open. ground_off must stay before the /* until */.
    assert_stream_matches_cold(
        "int a = 1;\n"
        "/* this comment\n"
        "   spans several\n"
        "   lines */\n"
        "int b = 2;\n",
        "cpp", "block-comment");

    // Triple-quoted string (Python) spanning lines.
    assert_stream_matches_cold(
        "x = 1\n"
        "doc = \"\"\"\n"
        "multi\n"
        "line\n"
        "\"\"\"\n"
        "y = 2\n",
        "python", "triple-quote");

    // Unterminated block comment at end-of-stream (the live tail is inside an
    // open comment for the whole back half of the stream).
    assert_stream_matches_cold(
        "ok();\n"
        "/* still open\n"
        "and open\n"
        "and still open",
        "cpp", "unterminated-comment");

    // Shell heredoc-ish content + comments (hash comments are single-line, so
    // ground state advances every line — exercises the common fast case).
    assert_stream_matches_cold(
        "#!/bin/sh\n"
        "# a comment\n"
        "echo \"$VAR\"\n"
        "for i in 1 2 3; do\n"
        "  echo $i\n"
        "done\n",
        "bash", "shell");

    // Long block to make the O(n) vs O(1) difference real, and to cross the
    // gutter's power-of-ten line boundary (9->10) while streaming.
    {
        std::string big;
        for (int i = 0; i < 30; ++i) {
            big += "let v";
            big += std::to_string(i);
            big += " = ";
            big += std::to_string(i * 7);
            big += ";\n";
        }
        assert_stream_matches_cold(big, "rust", "long-block-gutter-boundary");
    }

    std::println("All highlight-resume tests passed.");
    return 0;
}
