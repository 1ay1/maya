// syntax_internal.hpp — types + style palette shared between the language
// tables (syntax_lang.cpp) and the highlighter (syntax_highlight.cpp).
// Internal to the markdown render module; NOT installed for public use.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "maya/style/style.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {
namespace syntax_detail {

// ── Language identification ──────────────────────────────────────────────────
enum class LangId {
    Unknown,
    C, Cpp, Python, Rust, JavaScript, TypeScript, Go, Java, Kotlin, Swift,
    Ruby, Shell, Fish, SQL, HTML, XML, CSS, SCSS,
    JSON, YAML, TOML, Lua, Zig, Haskell, Elixir, Erlang, PHP, Perl, R,
    Diff, Makefile, CMake, Dockerfile, Markdown,
};

struct WordClass { bool keyword; bool type; bool constant; };

struct CommentStyle {
    const char* line;         // "//" or "#" or "--" or nullptr
    const char* block_open;   // "/*" or "{-" or nullptr
    const char* block_close;  // "*/" or "-}" or nullptr
    bool hash_comment;        // '#' as a line comment
};

struct LangFeatures {
    bool triple_quote_strings;  // Python """...""", '''...'''
    bool backtick_strings;      // JS `...`, Go raw strings
    bool preprocessor;          // C/C++ #include, #define
    bool decorators;            // Python/Java @, Rust #[...]
    bool shell_vars;            // $VAR, ${VAR}
    bool char_literals;         // 'c' is a char, not a string
    bool lifetime;              // Rust 'a lifetime annotations
    bool colon_atom;            // Ruby/Elixir :symbol
};

// Defined in syntax_lang.cpp.
[[nodiscard]] LangId       detect_lang(std::string_view tag);
[[nodiscard]] WordClass    classify_word(std::string_view word, LangId lang);
[[nodiscard]] CommentStyle comment_style_for(LangId lang);
[[nodiscard]] LangFeatures features_for(LangId lang);

[[nodiscard]] inline bool in_list(
    std::string_view word,
    std::initializer_list<std::string_view> list) {
    for (auto& k : list) if (word == k) return true;
    return false;
}

} // namespace syntax_detail

// ── Token style palette ──────────────────────────────────────────────────────
// Static const Styles constructed once, returned by reference. Used by both
// the diff highlighter and the language tokeniser.
namespace syntax {
    inline const Style& kw()       { static const Style s = Style{}.with_fg(Color::magenta()); return s; }
    inline const Style& ctrl()     { static const Style s = Style{}.with_fg(Color::magenta()); return s; }
    inline const Style& type()     { static const Style s = Style{}.with_fg(Color::cyan()); return s; }
    inline const Style& fn()       { static const Style s = Style{}.with_fg(Color::blue()); return s; }
    inline const Style& str()      { static const Style s = Style{}.with_fg(Color::green()); return s; }
    inline const Style& num()      { static const Style s = Style{}.with_fg(Color::bright_yellow()); return s; }
    inline const Style& comment()  { static const Style s = Style{}.with_fg(Color::bright_black()).with_italic(); return s; }
    inline const Style& constant() { static const Style s = Style{}.with_fg(Color::bright_yellow()); return s; }
    inline const Style& preproc()  { static const Style s = Style{}.with_fg(Color::yellow()); return s; }
    inline const Style& attr()     { static const Style s = Style{}.with_fg(Color::yellow()); return s; }
    inline const Style& op()       { static const Style s = Style{}.with_fg(Color::red()); return s; }
    inline const Style& punct()    { static const Style s = Style{}.with_fg(Color::bright_black()); return s; }
    inline const Style& plain()    { static const Style s = Style{}.with_fg(Color::white()); return s; }
    inline const Style& shellvar() { static const Style s = Style{}.with_fg(Color::bright_cyan()); return s; }
    inline const Style& gutter()   { static const Style s = Style{}.with_fg(Color::bright_black()).with_dim(); return s; }

    inline const Style& diff_add()  { static const Style s = Style{}.with_fg(Color::green()); return s; }
    inline const Style& diff_del()  { static const Style s = Style{}.with_fg(Color::red()); return s; }
    inline const Style& diff_hunk() { static const Style s = Style{}.with_fg(Color::cyan()); return s; }
    inline const Style& diff_meta() { static const Style s = Style{}.with_fg(Color::bright_black()).with_bold(); return s; }
}

} // namespace maya
