// Tests for maya::syntax — themeable highlight: capture spans + constexpr theme.
#include <maya/maya.hpp>
#include <maya/widget/markdown/highlight.hpp>
#include <cassert>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace maya;
using namespace maya::syntax;

// Find the capture assigned to the first occurrence of `needle` in `src`.
static Capture cap_of(std::string_view src, const std::vector<Span>& spans,
                      std::string_view needle) {
    auto pos = src.find(needle);
    if (pos == std::string_view::npos) return Capture::None;
    for (const auto& sp : spans) {
        if (sp.start <= pos && pos < sp.start + sp.len) return sp.cap;
    }
    return Capture::None;
}

// ── constexpr theme structure ───────────────────────────────────────────────
void test_theme_constexpr() {
    std::println("--- test_theme_constexpr ---");
    // The theme table is fully constexpr.
    static_assert(themes::monokai.style_for(Capture::Keyword).fg->kind()
                  == Color::Kind::Rgb);
    static_assert(themes::terminal.style_for(Capture::String).fg->kind()
                  == Color::Kind::Named);
    // `with()` builder is constexpr and overrides one capture.
    constexpr HighlightTheme custom =
        themes::terminal.with(Capture::Comment, Style{}.with_fg(Color::red()));
    static_assert(custom.style_for(Capture::Comment).fg->r() == Color::red().r());
    // Unmodified captures pass through.
    static_assert(custom.style_for(Capture::Keyword).fg->r()
                  == themes::terminal.style_for(Capture::Keyword).fg->r());
    std::println("PASS\n");
}

void test_lang_from_tag() {
    std::println("--- test_lang_from_tag ---");
    assert(lang_from_tag("cpp") == Lang::Cpp);
    assert(lang_from_tag("C++") == Lang::Cpp);
    assert(lang_from_tag("py") == Lang::Python);
    assert(lang_from_tag("Python") == Lang::Python);
    assert(lang_from_tag("rs") == Lang::Rust);
    assert(lang_from_tag("golang") == Lang::Go);
    assert(lang_from_tag("ts") == Lang::TypeScript);
    assert(lang_from_tag("bash") == Lang::Shell);
    assert(lang_from_tag("json") == Lang::Json);
    assert(lang_from_tag("brainfuck") == Lang::Generic);
    std::println("PASS\n");
}

// ── span contract: sorted, non-overlapping, in-bounds ───────────────────────
void test_span_contract() {
    std::println("--- test_span_contract ---");
    std::string src = R"(int main() {
    // entry point
    const char* msg = "hello, world";
    return 0;
})";
    auto spans = highlight(src, Lang::Cpp);
    assert(!spans.empty());
    std::uint32_t last_end = 0;
    for (const auto& sp : spans) {
        assert(sp.start >= last_end);                 // sorted, non-overlapping
        assert(sp.start + sp.len <= src.size());      // in-bounds
        assert(sp.len > 0);
        last_end = sp.start + sp.len;
    }
    std::println("PASS\n");
}

// ── C++ classification ──────────────────────────────────────────────────────
void test_cpp_captures() {
    std::println("--- test_cpp_captures ---");
    std::string src =
        "const int x = 0xFF; // note\n"
        "return func(\"str\");\n";
    auto spans = highlight(src, Lang::Cpp);

    assert(cap_of(src, spans, "const") == Capture::Keyword);
    assert(cap_of(src, spans, "int") == Capture::Type);
    assert(cap_of(src, spans, "0xFF") == Capture::Number);
    assert(cap_of(src, spans, "// note") == Capture::Comment);
    assert(cap_of(src, spans, "return") == Capture::KeywordCtrl);
    assert(cap_of(src, spans, "func") == Capture::Function);
    assert(cap_of(src, spans, "\"str\"") == Capture::String);
    std::println("PASS\n");
}

// ── Python classification ───────────────────────────────────────────────────
void test_python_captures() {
    std::println("--- test_python_captures ---");
    std::string src =
        "def greet(name):\n"
        "    # say hi\n"
        "    return f\"hi {name}\"\n"
        "x = None\n";
    auto spans = highlight(src, Lang::Python);

    assert(cap_of(src, spans, "def") == Capture::Keyword);
    assert(cap_of(src, spans, "# say hi") == Capture::Comment);
    assert(cap_of(src, spans, "return") == Capture::KeywordCtrl);
    assert(cap_of(src, spans, "None") == Capture::Constant);
    std::println("PASS\n");
}

// ── Rust attributes + types ─────────────────────────────────────────────────
void test_rust_captures() {
    std::println("--- test_rust_captures ---");
    std::string src =
        "#[derive(Debug)]\n"
        "fn make() -> Vec<u8> {\n"
        "    let v: Vec<u8> = vec![1, 2, 3];\n"
        "    v\n"
        "}\n";
    auto spans = highlight(src, Lang::Rust);

    assert(cap_of(src, spans, "fn") == Capture::Keyword);
    assert(cap_of(src, spans, "let") == Capture::Keyword);
    assert(cap_of(src, spans, "u8") == Capture::Type);
    // Vec is a known type.
    assert(cap_of(src, spans, "Vec") == Capture::Type);
    std::println("PASS\n");
}

// ── Shell variables ─────────────────────────────────────────────────────────
void test_shell_captures() {
    std::println("--- test_shell_captures ---");
    std::string src =
        "if [ -n $HOME ]; then\n"
        "    echo ${PATH}\n"
        "fi\n";
    auto spans = highlight(src, Lang::Shell);

    assert(cap_of(src, spans, "if") == Capture::KeywordCtrl);
    assert(cap_of(src, spans, "$HOME") == Capture::Variable);
    assert(cap_of(src, spans, "${PATH}") == Capture::Variable);
    std::println("PASS\n");
}

// ── Block comments don't swallow the rest of the file ───────────────────────
void test_block_comment_bounds() {
    std::println("--- test_block_comment_bounds ---");
    std::string src = "a /* comment */ b\n";
    auto spans = highlight(src, Lang::Cpp);
    // "b" appears after the comment closes; the comment span must end at */.
    Capture cb = cap_of(src, spans, "comment");
    assert(cb == Capture::Comment);
    // The 'b' identifier (plain) is NOT inside any comment span.
    auto bpos = src.rfind('b');
    for (const auto& sp : spans) {
        if (sp.cap == Capture::Comment)
            assert(!(sp.start <= bpos && bpos < sp.start + sp.len));
    }
    std::println("PASS\n");
}

// ── Empty input is handled ──────────────────────────────────────────────────
void test_empty() {
    std::println("--- test_empty ---");
    auto spans = highlight("", Lang::Cpp);
    assert(spans.empty());
    std::println("PASS\n");
}

int main() {
    test_theme_constexpr();
    test_lang_from_tag();
    test_span_contract();
    test_cpp_captures();
    test_python_captures();
    test_rust_captures();
    test_shell_captures();
    test_block_comment_bounds();
    test_empty();
    std::println("All highlight tests passed.");
    return 0;
}
