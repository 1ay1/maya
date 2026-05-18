// syntax.cpp — Language-aware code-block syntax highlighter.
//
// Owns: namespace syntax (token styles), LangId + detect_lang,
// classify_word, comment_style_for, features_for, highlight_diff,
// highlight_code_impl (main tokenizer + gutter pass), and the
// thread-local FNV-1a memo wrapper highlight_code().
//
// Calls find_eol via maya::md_detail (parser.cpp owns the definition).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {

// Local alias so the body of highlight_code_impl (which calls find_eol
// unqualified) resolves to parser.cpp's implementation.
using ::maya::md_detail::find_eol;

namespace {

// Compile-time char tables — duplicated from parser.cpp so the inner
// loops can inline kFoo[c] without cross-TU indirection. Each table is
// 256 B of rodata.
struct CharTable {
    bool v[256]{};
    constexpr bool operator[](unsigned char c) const noexcept { return v[c]; }
};

template <unsigned char... Cs>
consteval CharTable make_table() {
    CharTable t{};
    ((t.v[Cs] = true), ...);
    return t;
}

static constexpr auto kPunctChar = make_table<
    '{', '}', '[', ']', '(', ')', '.', ',', ';', ':',
    '<', '>', '?', '~', '%', '@', '\\'>();

static constexpr auto kOpChar = make_table<
    '+', '-', '*', '/', '=', '!', '&', '|', '^'>();

} // anonymous

// ============================================================================
// Language-aware syntax highlighting for code blocks
// ============================================================================
// Uses only terminal named ANSI colors so highlighting adapts to the user's
// terminal theme (Catppuccin, Dracula, Solarized, One Dark, Gruvbox, etc.)

namespace syntax {
    // Static const: constructed once, returned by reference — avoids
    // rebuilding Style objects on every token emission.
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
    // Gutter line-number column for code blocks ≥ 5 lines. dim +
    // bright_black so it sits visually behind the code without
    // competing with the syntax-highlighted body.
    inline const Style& gutter()   { static const Style s = Style{}.with_fg(Color::bright_black()).with_dim(); return s; }

    // Diff highlighting
    inline const Style& diff_add()  { static const Style s = Style{}.with_fg(Color::green()); return s; }
    inline const Style& diff_del()  { static const Style s = Style{}.with_fg(Color::red()); return s; }
    inline const Style& diff_hunk() { static const Style s = Style{}.with_fg(Color::cyan()); return s; }
    inline const Style& diff_meta() { static const Style s = Style{}.with_fg(Color::bright_black()).with_bold(); return s; }
}

// ── Language identification ──────────────────────────────────────────────────

enum class LangId {
    Unknown,
    C, Cpp, Python, Rust, JavaScript, TypeScript, Go, Java, Kotlin, Swift,
    Ruby, Shell, Fish, SQL, HTML, XML, CSS, SCSS,
    JSON, YAML, TOML, Lua, Zig, Haskell, Elixir, Erlang, PHP, Perl, R,
    Diff, Makefile, CMake, Dockerfile, Markdown,
};

static LangId detect_lang(std::string_view tag) {
    // Normalize: lowercase — use stack buffer to avoid heap allocation
    // (language tags are always short, typically < 16 chars).
    char buf[32];
    size_t len = std::min(tag.size(), sizeof(buf) - 1);
    for (size_t k = 0; k < len; ++k)
        buf[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[k])));
    buf[len] = '\0';
    std::string_view lower{buf, len};

    if (lower == "c" || lower == "h")                return LangId::C;
    if (lower == "cpp" || lower == "c++" ||
        lower == "cxx" || lower == "cc" ||
        lower == "hpp" || lower == "hxx")            return LangId::Cpp;
    if (lower == "python" || lower == "py")          return LangId::Python;
    if (lower == "rust" || lower == "rs")            return LangId::Rust;
    if (lower == "javascript" || lower == "js" ||
        lower == "jsx" || lower == "mjs" ||
        lower == "cjs")                              return LangId::JavaScript;
    if (lower == "typescript" || lower == "ts" ||
        lower == "tsx")                              return LangId::TypeScript;
    if (lower == "go" || lower == "golang")          return LangId::Go;
    if (lower == "java")                             return LangId::Java;
    if (lower == "kotlin" || lower == "kt" ||
        lower == "kts")                              return LangId::Kotlin;
    if (lower == "swift")                            return LangId::Swift;
    if (lower == "ruby" || lower == "rb")            return LangId::Ruby;
    if (lower == "bash" || lower == "sh" ||
        lower == "shell" || lower == "zsh")          return LangId::Shell;
    if (lower == "fish")                             return LangId::Fish;
    if (lower == "sql" || lower == "mysql" ||
        lower == "postgresql" || lower == "sqlite")  return LangId::SQL;
    if (lower == "html" || lower == "htm")           return LangId::HTML;
    if (lower == "xml" || lower == "svg")            return LangId::XML;
    if (lower == "css")                              return LangId::CSS;
    if (lower == "scss" || lower == "sass" ||
        lower == "less")                             return LangId::SCSS;
    if (lower == "json" || lower == "jsonc")         return LangId::JSON;
    if (lower == "yaml" || lower == "yml")           return LangId::YAML;
    if (lower == "toml")                             return LangId::TOML;
    if (lower == "lua")                              return LangId::Lua;
    if (lower == "zig")                              return LangId::Zig;
    if (lower == "haskell" || lower == "hs")         return LangId::Haskell;
    if (lower == "elixir" || lower == "ex" ||
        lower == "exs")                              return LangId::Elixir;
    if (lower == "erlang" || lower == "erl")         return LangId::Erlang;
    if (lower == "php")                              return LangId::PHP;
    if (lower == "perl" || lower == "pl")            return LangId::Perl;
    if (lower == "r")                                return LangId::R;
    if (lower == "diff" || lower == "patch")         return LangId::Diff;
    if (lower == "makefile" || lower == "make")      return LangId::Makefile;
    if (lower == "cmake")                            return LangId::CMake;
    if (lower == "dockerfile" || lower == "docker")  return LangId::Dockerfile;
    if (lower == "markdown" || lower == "md")        return LangId::Markdown;
    return LangId::Unknown;
}

// ── Per-language keyword tables ──────────────────────────────────────────────

static bool in_list(std::string_view word, std::initializer_list<std::string_view> list) {
    for (auto& k : list) if (word == k) return true;
    return false;
}

struct WordClass { bool keyword; bool type; bool constant; };

static WordClass classify_word(std::string_view word, LangId lang) {
    // Constants — universal
    if (in_list(word, {"true", "false", "null", "nullptr", "None", "nil",
                       "True", "False", "NULL", "NaN", "Infinity",
                       "undefined", "NUL", "YES", "NO"}))
        return {false, false, true};

    switch (lang) {
    case LangId::C:
    case LangId::Cpp:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "goto", "default",
            "struct", "enum", "union", "typedef", "class", "namespace",
            "template", "typename", "using", "static", "extern", "inline",
            "const", "constexpr", "consteval", "constinit", "volatile",
            "mutable", "register", "thread_local",
            "virtual", "override", "final", "explicit", "noexcept",
            "public", "private", "protected", "friend",
            "new", "delete", "operator", "sizeof", "alignof", "decltype",
            "static_assert", "static_cast", "dynamic_cast", "reinterpret_cast",
            "const_cast", "typeid", "throw", "try", "catch",
            "concept", "requires", "co_await", "co_yield", "co_return",
            "export", "import", "module",
            "auto", "void",
            "#include", "#define", "#ifdef", "#ifndef", "#endif", "#pragma",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "char", "float", "double", "long", "short", "unsigned",
            "signed", "bool", "size_t", "uint8_t", "uint16_t", "uint32_t",
            "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
            "string", "string_view", "vector", "map", "set", "array",
            "optional", "variant", "pair", "tuple", "span", "expected",
            "unique_ptr", "shared_ptr", "weak_ptr",
            "wchar_t", "char8_t", "char16_t", "char32_t", "ptrdiff_t",
        })) return {false, true, false};
        break;

    case LangId::Python:
        if (in_list(word, {
            "if", "elif", "else", "for", "while", "break", "continue",
            "return", "yield", "pass", "raise", "try", "except", "finally",
            "with", "as", "assert", "del",
            "def", "class", "lambda", "async", "await",
            "import", "from", "global", "nonlocal",
            "and", "or", "not", "in", "is",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "float", "str", "bool", "list", "dict", "tuple", "set",
            "bytes", "bytearray", "complex", "frozenset", "type", "object",
            "range", "enumerate", "zip", "map", "filter",
            "Exception", "ValueError", "TypeError", "KeyError", "IndexError",
            "RuntimeError", "StopIteration", "AttributeError", "ImportError",
            "OSError", "IOError", "FileNotFoundError",
        })) return {false, true, false};
        if (in_list(word, {"self", "cls", "super"}))
            return {true, false, false};
        break;

    case LangId::Rust:
        if (in_list(word, {
            "if", "else", "for", "while", "loop", "break", "continue",
            "return", "match", "as",
            "fn", "struct", "enum", "impl", "trait", "type", "where",
            "let", "mut", "const", "static", "ref", "move",
            "pub", "mod", "use", "crate", "super", "self", "Self",
            "async", "await", "unsafe", "extern", "dyn",
        })) return {true, false, false};
        if (in_list(word, {
            "i8", "i16", "i32", "i64", "i128", "isize",
            "u8", "u16", "u32", "u64", "u128", "usize",
            "f32", "f64", "bool", "char", "str",
            "String", "Vec", "Box", "Rc", "Arc", "Cell", "RefCell",
            "Option", "Result", "Ok", "Err", "Some",
            "HashMap", "HashSet", "BTreeMap", "BTreeSet",
            "Iterator", "IntoIterator", "From", "Into",
            "Display", "Debug", "Clone", "Copy", "Send", "Sync",
            "Default", "PartialEq", "Eq", "PartialOrd", "Ord", "Hash",
        })) return {false, true, false};
        break;

    case LangId::JavaScript:
    case LangId::TypeScript:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "throw", "try", "catch", "finally",
            "default", "in", "of", "typeof", "instanceof", "void", "delete",
            "function", "class", "extends", "new", "this", "super",
            "const", "let", "var", "async", "await", "yield",
            "import", "export", "from", "as", "default",
            "with", "debugger",
        })) return {true, false, false};
        if (lang == LangId::TypeScript && in_list(word, {
            "type", "interface", "enum", "namespace", "declare", "abstract",
            "implements", "readonly", "keyof", "infer", "is", "asserts",
            "override", "satisfies",
        })) return {true, false, false};
        if (in_list(word, {
            "string", "number", "boolean", "object", "symbol", "bigint",
            "any", "unknown", "never", "void",
            "Array", "Map", "Set", "Promise", "Date", "RegExp", "Error",
            "Object", "Function", "Symbol", "WeakMap", "WeakSet",
            "Record", "Partial", "Required", "Readonly", "Pick", "Omit",
        })) return {false, true, false};
        break;

    case LangId::Go:
        if (in_list(word, {
            "if", "else", "for", "switch", "case", "break", "continue",
            "return", "goto", "default", "fallthrough", "select",
            "func", "type", "struct", "interface", "map", "chan",
            "var", "const", "package", "import",
            "go", "defer", "range",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
            "float32", "float64", "complex64", "complex128",
            "bool", "byte", "rune", "string", "error",
            "any", "comparable",
        })) return {false, true, false};
        if (in_list(word, {"make", "append", "len", "cap", "copy", "close",
                           "new", "delete", "panic", "recover", "print", "println",
                           "iota"}))
            return {false, false, true};
        break;

    case LangId::Java:
    case LangId::Kotlin:
        if (in_list(word, {
            "if", "else", "for", "while", "do", "switch", "case", "break",
            "continue", "return", "throw", "try", "catch", "finally",
            "default", "instanceof", "new", "this", "super",
            "class", "interface", "enum", "extends", "implements",
            "abstract", "final", "static", "synchronized", "volatile",
            "transient", "native", "strictfp",
            "public", "private", "protected", "package", "import",
            "assert", "throws", "void",
        })) return {true, false, false};
        if (lang == LangId::Kotlin && in_list(word, {
            "fun", "val", "var", "when", "is", "as", "in", "out",
            "object", "companion", "data", "sealed", "inline", "reified",
            "suspend", "override", "open", "internal", "lateinit",
            "by", "constructor", "init", "typealias",
        })) return {true, false, false};
        if (in_list(word, {
            "int", "long", "short", "byte", "float", "double", "char",
            "boolean", "String", "Integer", "Long", "Double", "Float",
            "Boolean", "Character", "Object", "Void",
            "List", "Map", "Set", "Array", "Collection", "Iterator",
            "Optional", "Stream", "Comparable", "Iterable",
        })) return {false, true, false};
        break;

    case LangId::Ruby:
        if (in_list(word, {
            "if", "elsif", "else", "unless", "while", "until", "for",
            "do", "end", "begin", "rescue", "ensure", "raise", "retry",
            "return", "break", "next", "redo", "yield",
            "def", "class", "module", "include", "extend", "require",
            "require_relative", "attr_reader", "attr_writer", "attr_accessor",
            "self", "super", "then", "when", "case", "in", "and", "or", "not",
            "defined?", "alias", "undef", "private", "protected", "public",
            "lambda", "proc", "block_given?",
        })) return {true, false, false};
        break;

    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:
        if (in_list(word, {
            "if", "then", "else", "elif", "fi", "for", "while", "until",
            "do", "done", "case", "esac", "in", "function", "return",
            "local", "export", "unset", "readonly", "declare", "typeset",
            "source", "eval", "exec", "set", "shift", "trap",
            "echo", "printf", "read", "test", "exit",
            // Dockerfile
            "FROM", "RUN", "CMD", "ENTRYPOINT", "COPY", "ADD", "WORKDIR",
            "ENV", "ARG", "EXPOSE", "VOLUME", "USER", "LABEL", "ONBUILD",
            "HEALTHCHECK", "SHELL", "STOPSIGNAL",
        })) return {true, false, false};
        break;

    case LangId::SQL:
        if (in_list(word, {
            "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE",
            "SET", "DELETE", "CREATE", "ALTER", "DROP", "TABLE", "INDEX",
            "VIEW", "DATABASE", "SCHEMA", "JOIN", "LEFT", "RIGHT", "INNER",
            "OUTER", "FULL", "CROSS", "ON", "AS", "AND", "OR", "NOT", "IN",
            "IS", "LIKE", "BETWEEN", "EXISTS", "HAVING", "GROUP", "BY",
            "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "UNION", "ALL",
            "DISTINCT", "CASE", "WHEN", "THEN", "ELSE", "END", "IF",
            "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION", "GRANT", "REVOKE",
            "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "CONSTRAINT",
            "UNIQUE", "CHECK", "DEFAULT", "CASCADE", "RESTRICT",
            // Also match lowercase
            "select", "from", "where", "insert", "into", "values", "update",
            "set", "delete", "create", "alter", "drop", "table", "index",
            "view", "database", "schema", "join", "left", "right", "inner",
            "outer", "full", "cross", "on", "as", "and", "or", "not", "in",
            "is", "like", "between", "exists", "having", "group", "by",
            "order", "asc", "desc", "limit", "offset", "union", "all",
            "distinct", "case", "when", "then", "else", "end", "if",
            "begin", "commit", "rollback", "transaction", "grant", "revoke",
            "primary", "key", "foreign", "references", "constraint",
            "unique", "check", "default", "cascade", "restrict",
        })) return {true, false, false};
        if (in_list(word, {
            "INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT",
            "VARCHAR", "CHAR", "TEXT", "BLOB", "BOOLEAN", "BOOL",
            "FLOAT", "DOUBLE", "DECIMAL", "NUMERIC", "REAL",
            "DATE", "TIME", "TIMESTAMP", "DATETIME",
            "SERIAL", "BIGSERIAL", "UUID",
            "int", "integer", "bigint", "smallint", "tinyint",
            "varchar", "char", "text", "blob", "boolean", "bool",
            "float", "double", "decimal", "numeric", "real",
            "date", "time", "timestamp", "datetime",
            "serial", "bigserial", "uuid",
        })) return {false, true, false};
        break;

    case LangId::Lua:
        if (in_list(word, {
            "if", "then", "else", "elseif", "end", "for", "while", "do",
            "repeat", "until", "break", "return", "goto",
            "function", "local", "in", "and", "or", "not",
        })) return {true, false, false};
        break;

    case LangId::Zig:
        if (in_list(word, {
            "if", "else", "for", "while", "break", "continue", "return",
            "switch", "orelse", "catch", "unreachable",
            "fn", "pub", "const", "var", "struct", "enum", "union",
            "error", "test", "comptime", "inline", "extern", "export",
            "threadlocal", "defer", "errdefer", "nosuspend",
            "try", "async", "await", "suspend", "resume",
            "align", "allowzero", "volatile", "linksection",
        })) return {true, false, false};
        if (in_list(word, {
            "u8", "u16", "u32", "u64", "u128", "usize",
            "i8", "i16", "i32", "i64", "i128", "isize",
            "f16", "f32", "f64", "f128", "bool", "void", "noreturn",
            "anyerror", "anyframe", "anytype", "anyopaque", "type",
            "comptime_int", "comptime_float",
        })) return {false, true, false};
        break;

    case LangId::Swift:
        if (in_list(word, {
            "if", "else", "for", "while", "repeat", "switch", "case",
            "break", "continue", "return", "throw", "guard", "defer",
            "do", "try", "catch", "where", "in", "as", "is",
            "func", "class", "struct", "enum", "protocol", "extension",
            "typealias", "associatedtype",
            "let", "var", "static", "lazy", "override", "mutating",
            "public", "private", "internal", "fileprivate", "open",
            "import", "init", "deinit", "subscript", "operator",
            "async", "await", "actor",
            "self", "Self", "super",
        })) return {true, false, false};
        if (in_list(word, {
            "Int", "Int8", "Int16", "Int32", "Int64",
            "UInt", "UInt8", "UInt16", "UInt32", "UInt64",
            "Float", "Double", "Bool", "String", "Character",
            "Array", "Dictionary", "Set", "Optional",
            "Any", "AnyObject", "Void", "Never",
        })) return {false, true, false};
        break;

    case LangId::Haskell:
        if (in_list(word, {
            "if", "then", "else", "case", "of", "let", "in", "where",
            "do", "module", "import", "data", "type", "newtype", "class",
            "instance", "deriving", "default", "forall", "infixl", "infixr",
            "infix", "qualified", "as", "hiding",
        })) return {true, false, false};
        if (in_list(word, {
            "Int", "Integer", "Float", "Double", "Char", "String", "Bool",
            "IO", "Maybe", "Either", "Monad", "Functor", "Applicative",
            "Show", "Read", "Eq", "Ord", "Num", "Enum", "Bounded",
        })) return {false, true, false};
        break;

    case LangId::Elixir:
    case LangId::Erlang:
        if (in_list(word, {
            "if", "else", "do", "end", "case", "cond", "when", "with",
            "for", "unless", "fn", "def", "defp", "defmodule", "defstruct",
            "defimpl", "defprotocol", "defmacro", "defguard",
            "import", "require", "use", "alias", "raise", "rescue",
            "try", "catch", "after", "receive", "send", "spawn",
            "and", "or", "not", "in",
        })) return {true, false, false};
        break;

    case LangId::PHP:
        if (in_list(word, {
            "if", "else", "elseif", "for", "foreach", "while", "do",
            "switch", "case", "break", "continue", "return", "throw",
            "try", "catch", "finally", "default",
            "function", "class", "interface", "trait", "extends", "implements",
            "abstract", "final", "static", "public", "private", "protected",
            "new", "instanceof", "as", "use", "namespace", "echo", "print",
            "include", "require", "include_once", "require_once",
            "yield", "fn", "match", "enum", "readonly",
        })) return {true, false, false};
        break;

    case LangId::CSS:
    case LangId::SCSS:
    case LangId::HTML:
    case LangId::XML:
    case LangId::JSON:
    case LangId::YAML:
    case LangId::TOML:
    case LangId::Perl:
    case LangId::R:
    case LangId::CMake:
    case LangId::Markdown:
    case LangId::Unknown:
        break;
    }

    return {false, false, false};
}

// ── Comment style per language ───────────────────────────────────────────────

struct CommentStyle {
    const char* line;         // "//" or "#" or "--" or nullptr
    const char* block_open;   // "/*" or "{-" or nullptr
    const char* block_close;  // "*/" or "-}" or nullptr
    bool hash_comment;        // '#' as line comment (separate from "//" because
                              // '#' can also be preprocessor in C/C++)
};

static CommentStyle comment_style_for(LangId lang) {
    switch (lang) {
    case LangId::C:
    case LangId::Cpp:          return {"//", "/*", "*/", false};
    case LangId::Python:       return {nullptr, nullptr, nullptr, true};
    case LangId::Rust:         return {"//", "/*", "*/", false};
    case LangId::JavaScript:
    case LangId::TypeScript:   return {"//", "/*", "*/", false};
    case LangId::Go:           return {"//", "/*", "*/", false};
    case LangId::Java:
    case LangId::Kotlin:       return {"//", "/*", "*/", false};
    case LangId::Swift:        return {"//", "/*", "*/", false};
    case LangId::Zig:          return {"//", nullptr, nullptr, false};
    case LangId::Lua:          return {"--", "--[[", "]]", false};
    case LangId::Haskell:      return {"--", "{-", "-}", false};
    case LangId::SQL:          return {"--", "/*", "*/", false};
    case LangId::Ruby:         return {nullptr, "=begin", "=end", true};
    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:
    case LangId::YAML:
    case LangId::TOML:
    case LangId::R:
    case LangId::CMake:
    case LangId::Elixir:
    case LangId::Erlang:       return {nullptr, nullptr, nullptr, true};
    case LangId::PHP:          return {"//", "/*", "*/", true};
    case LangId::Perl:         return {nullptr, nullptr, nullptr, true};
    case LangId::CSS:
    case LangId::SCSS:         return {"//", "/*", "*/", false};
    case LangId::HTML:
    case LangId::XML:          return {nullptr, "<!--", "-->", false};
    case LangId::JSON:         return {nullptr, nullptr, nullptr, false};
    case LangId::Diff:
    case LangId::Markdown:
    case LangId::Unknown:      return {"//", "/*", "*/", true};
    }
    return {"//", "/*", "*/", true};
}

// ── Language feature flags ───────────────────────────────────────────────────

struct LangFeatures {
    bool triple_quote_strings;  // Python """...""", '''...'''
    bool backtick_strings;      // JS `...` template literals, Go raw strings
    bool preprocessor;          // C/C++ #include, #define
    bool decorators;            // Python @, Java @, Rust #[...]
    bool shell_vars;            // $VAR, ${VAR}
    bool char_literals;         // 'c' is a char, not a string
    bool lifetime;              // Rust 'a lifetime annotations
    bool colon_atom;            // Ruby/Elixir :symbol
};

static LangFeatures features_for(LangId lang) {
    switch (lang) {
    case LangId::C:
    case LangId::Cpp:          return {false, false, true,  false, false, true,  false, false};
    case LangId::Python:       return {true,  false, false, true,  false, false, false, false};
    case LangId::Rust:         return {false, false, false, false, false, false, true,  false};
    case LangId::JavaScript:   return {false, true,  false, true,  false, false, false, false};
    case LangId::TypeScript:   return {false, true,  false, true,  false, false, false, false};
    case LangId::Go:           return {false, true,  false, false, false, true,  false, false};
    case LangId::Java:         return {false, false, false, true,  false, true,  false, false};
    case LangId::Kotlin:       return {false, false, false, true,  false, true,  false, false};
    case LangId::Swift:        return {false, false, false, true,  false, false, false, false};
    case LangId::Ruby:         return {false, false, false, false, false, false, false, true};
    case LangId::Shell:
    case LangId::Fish:
    case LangId::Makefile:
    case LangId::Dockerfile:   return {false, false, false, false, true,  false, false, false};
    case LangId::PHP:          return {false, false, false, false, true,  true,  false, false};
    case LangId::Perl:         return {false, false, false, false, true,  false, false, false};
    case LangId::Elixir:       return {false, false, false, true,  false, false, false, true};
    case LangId::Erlang:       return {false, false, false, false, false, true,  false, false};
    default:                   return {false, false, false, false, false, false, false, false};
    }
}

static Element highlight_diff(const std::string& code) {
    std::string out;
    out.reserve(code.size());
    std::vector<StyledRun> runs;

    size_t i = 0;
    size_t n = code.size();

    while (i < n) {
        size_t line_start = i;
        // Find end of line (memchr is SIMD-accelerated in glibc)
        size_t eol = find_eol(code.data(), i, n);
        bool has_nl = (eol < n);
        size_t line_end = has_nl ? eol + 1 : eol;

        std::string_view line{code.data() + line_start, eol - line_start};
        size_t out_start = out.size();
        out.append(code, line_start, line_end - line_start);

        Style sty = syntax::plain();
        if (!line.empty()) {
            if (line[0] == '+')                                    sty = syntax::diff_add();
            else if (line[0] == '-')                               sty = syntax::diff_del();
            else if (line.starts_with("@@"))                       sty = syntax::diff_hunk();
            else if (line.starts_with("diff ") ||
                     line.starts_with("index ") ||
                     line.starts_with("--- ") ||
                     line.starts_with("+++ "))                     sty = syntax::diff_meta();
        }
        runs.push_back({out_start, line_end - line_start, sty});
        i = line_end;
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = syntax::plain(),
        // Same rationale as highlight_code_impl’s return: diff
        // lines are atomic and must not soft-wrap into the gutter.
        .wrap = TextWrap::NoWrap,
        .runs = std::move(runs),
    }};
}

static inline bool is_punct_char(char c) {
    return kPunctChar[static_cast<unsigned char>(c)];
}

static inline bool is_op_char(char c) {
    return kOpChar[static_cast<unsigned char>(c)];
}

static Element highlight_code_impl(const std::string& code, const std::string& lang_tag) {
    LangId lang = detect_lang(lang_tag);

    // Special case: diff gets its own highlighter
    if (lang == LangId::Diff) return highlight_diff(code);

    auto cs = comment_style_for(lang);
    auto feat = features_for(lang);

    std::string out;
    out.reserve(code.size());
    std::vector<StyledRun> runs;

    size_t i = 0;
    size_t n = code.size();

    auto emit = [&](size_t start, size_t byte_len, Style sty) {
        if (byte_len == 0) return;
        runs.push_back({start, byte_len, sty});
    };

    while (i < n) {
        char ch = code[i];

        // ── Newline ──────────────────────────────────────────────────
        if (ch == '\n') {
            size_t s = out.size();
            out += '\n';
            emit(s, 1, syntax::plain());
            ++i;
            continue;
        }

        // ── Whitespace ───────────────────────────────────────────────
        if (ch == ' ' || ch == '\t') {
            size_t s = out.size();
            size_t j = i;
            while (j < n && (code[j] == ' ' || code[j] == '\t')) ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::plain());
            i = j;
            continue;
        }

        // ── Preprocessor: #include, #define, etc. ────────────────────
        if (feat.preprocessor && ch == '#') {
            // Check if at start of line (or start of code)
            bool at_line_start = (i == 0 || code[i - 1] == '\n');
            if (at_line_start) {
                size_t s = out.size();
                size_t j = find_eol(code.data(), i, n);
                out.append(code, i, j - i);
                emit(s, j - i, syntax::preproc());
                i = j;
                continue;
            }
        }

        // ── Line comment: // or # or -- ──────────────────────────────
        if (cs.line && !std::string_view(cs.line).empty()) {
            std::string_view lc{cs.line};
            if (code.compare(i, lc.size(), lc) == 0) {
                size_t s = out.size();
                size_t j = find_eol(code.data(), i, n);
                out.append(code, i, j - i);
                emit(s, j - i, syntax::comment());
                i = j;
                continue;
            }
        }
        if (cs.hash_comment && ch == '#') {
            size_t s = out.size();
            size_t j = find_eol(code.data(), i, n);
            out.append(code, i, j - i);
            emit(s, j - i, syntax::comment());
            i = j;
            continue;
        }

        // ── Block comment: /* ... */, <!-- ... -->, {- ... -} ────────
        if (cs.block_open) {
            std::string_view bo{cs.block_open};
            std::string_view bc{cs.block_close};
            if (code.compare(i, bo.size(), bo) == 0) {
                size_t s = out.size();
                size_t j = i + bo.size();
                while (j + bc.size() <= n &&
                       code.compare(j, bc.size(), bc) != 0)
                    ++j;
                if (j + bc.size() <= n) j += bc.size();
                out.append(code, i, j - i);
                emit(s, j - i, syntax::comment());
                i = j;
                continue;
            }
        }

        // ── Decorators/attributes: @decorator, #[attr] ──────────────
        if (feat.decorators && ch == '@') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_' || code[j] == '.'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::attr());
            i = j;
            continue;
        }

        // ── Rust lifetime: 'a, 'static ──────────────────────────────
        if (feat.lifetime && ch == '\'' &&
            i + 1 < n && std::isalpha(static_cast<unsigned char>(code[i + 1]))) {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::type());
            i = j;
            continue;
        }

        // ── Shell variables: $VAR, ${VAR}, $(...) ────────────────────
        if (feat.shell_vars && ch == '$' && i + 1 < n) {
            size_t s = out.size();
            size_t j = i + 1;
            if (code[j] == '{') {
                // ${VAR}
                ++j;
                while (j < n && code[j] != '}') ++j;
                if (j < n) ++j;
            } else if (code[j] == '(') {
                // $(...) — just highlight the $( and )
                out.append(code, i, 2);
                emit(s, 2, syntax::shellvar());
                i += 2;
                continue;
            } else if (std::isalpha(static_cast<unsigned char>(code[j])) ||
                       code[j] == '_') {
                while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                                  code[j] == '_'))
                    ++j;
            } else {
                // $? $# $@ etc.
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::shellvar());
            i = j;
            continue;
        }

        // ── Triple-quoted strings: """...""" / '''...''' ─────────────
        if (feat.triple_quote_strings &&
            (ch == '"' || ch == '\'') &&
            i + 2 < n && code[i + 1] == ch && code[i + 2] == ch) {
            char q = ch;
            size_t s = out.size();
            size_t j = i + 3;
            while (j + 2 < n) {
                if (code[j] == '\\') { j += 2; continue; }
                if (code[j] == q && code[j + 1] == q && code[j + 2] == q) {
                    j += 3;
                    break;
                }
                ++j;
            }
            if (j + 2 >= n && !(j >= 3 && code[j-1] == q && code[j-2] == q && code[j-3] == q))
                j = n;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Backtick template literals: `...${...}...` ──────────────
        if (feat.backtick_strings && ch == '`') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '`') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                ++j;
            }
            if (j < n) ++j; // consume closing `
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── String literals: "..." ───────────────────────────────────
        if (ch == '"') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == '"') { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Char literal or string: '...' ────────────────────────────
        if (ch == '\'') {
            if (feat.char_literals) {
                // C-style char: 'x' or '\n' — short
                size_t s = out.size();
                size_t j = i + 1;
                if (j < n && code[j] == '\\') j += 2;
                else if (j < n) ++j;
                if (j < n && code[j] == '\'') ++j;
                out.append(code, i, j - i);
                emit(s, j - i, syntax::str());
                i = j;
                continue;
            }
            // Treat as string in Ruby, Python (single-quoted), etc.
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == '\'') { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Ruby/Elixir atom: :symbol ────────────────────────────────
        if (feat.colon_atom && ch == ':' &&
            i + 1 < n && std::isalpha(static_cast<unsigned char>(code[i + 1]))) {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_' || code[j] == '?' || code[j] == '!'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::constant());
            i = j;
            continue;
        }

        // ── Number: 0x..., 0b..., 0o..., digits[.digits][e...] ──────
        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            (ch == '.' && i + 1 < n &&
             std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
            size_t s = out.size();
            size_t j = i;
            if (ch == '0' && j + 1 < n) {
                char next = code[j + 1];
                if (next == 'x' || next == 'X') {
                    j += 2;
                    while (j < n && (std::isxdigit(static_cast<unsigned char>(code[j])) ||
                                      code[j] == '_'))
                        ++j;
                } else if (next == 'b' || next == 'B') {
                    j += 2;
                    while (j < n && (code[j] == '0' || code[j] == '1' || code[j] == '_'))
                        ++j;
                } else if (next == 'o' || next == 'O') {
                    j += 2;
                    while (j < n && ((code[j] >= '0' && code[j] <= '7') || code[j] == '_'))
                        ++j;
                } else goto decimal;
            } else {
            decimal:
                while (j < n && (std::isdigit(static_cast<unsigned char>(code[j])) ||
                                  code[j] == '_'))
                    ++j;
                if (j < n && code[j] == '.' && j + 1 < n &&
                    std::isdigit(static_cast<unsigned char>(code[j + 1]))) {
                    ++j;
                    while (j < n && (std::isdigit(static_cast<unsigned char>(code[j])) ||
                                      code[j] == '_'))
                        ++j;
                }
                // Exponent
                if (j < n && (code[j] == 'e' || code[j] == 'E')) {
                    ++j;
                    if (j < n && (code[j] == '+' || code[j] == '-')) ++j;
                    while (j < n && std::isdigit(static_cast<unsigned char>(code[j]))) ++j;
                }
            }
            // Number suffix: f, u, l, i32, etc.
            while (j < n && (std::isalpha(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::num());
            i = j;
            continue;
        }

        // ── Identifier / keyword / type / function ───────────────────
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            // Rust macros: name!
            if (lang == LangId::Rust && j < n && code[j] == '!')
                ++j;

            std::string_view word{code.data() + i, j - i};
            bool is_fn_call = (j < n && code[j] == '(');

            size_t s = out.size();
            out.append(code, i, j - i);

            auto wc = classify_word(word, lang);
            if (wc.constant)      emit(s, j - i, syntax::constant());
            else if (wc.keyword)  emit(s, j - i, syntax::kw());
            else if (wc.type)     emit(s, j - i, syntax::type());
            else if (is_fn_call)  emit(s, j - i, syntax::fn());
            else if (!word.empty() &&
                     std::isupper(static_cast<unsigned char>(word[0])) &&
                     word.size() > 1)
                                  emit(s, j - i, syntax::type());
            else                  emit(s, j - i, syntax::plain());

            i = j;
            continue;
        }

        // ── Multi-char operators: =>, ->, ::, |>, <-, ==, != etc. ───
        if (is_op_char(ch)) {
            size_t s = out.size();
            size_t j = i;
            // Consume runs of operator chars (max 3 for things like >>=)
            while (j < n && is_op_char(code[j]) && (j - i) < 3) ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::op());
            i = j;
            continue;
        }

        // ── Punctuation ──────────────────────────────────────────────
        if (is_punct_char(ch)) {
            size_t s = out.size();
            out += ch;
            emit(s, 1, syntax::punct());
            ++i;
            continue;
        }

        // ── Anything else — plain ────────────────────────────────────
        {
            size_t s = out.size();
            // Consume UTF-8 continuation bytes together
            size_t j = i + 1;
            if (static_cast<unsigned char>(ch) >= 0x80) {
                while (j < n && (static_cast<unsigned char>(code[j]) & 0xC0) == 0x80) ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::plain());
            i = j;
        }
    }

    // ── Gutter pass: prepend a right-aligned line-number column to each
    //    line. Height of the rendered block is unchanged — exactly one
    //    line out per line in, so monotonicity is preserved (a code
    //    fence that committed at K rows still commits at K rows).
    //
    //    Skip on small blocks (< 5 lines): the gutter is visual noise
    //    on a 2-line snippet, where the line numbers are obvious from
    //    position alone. The threshold matches what users intuitively
    //    expect — a "code listing" rather than a "snippet".
    {
        auto count_lines = [](std::string_view s) -> int {
            int n = 0;
            for (char c : s) if (c == '\n') ++n;
            if (!s.empty() && s.back() != '\n') ++n;
            return n;
        };
        const int line_count = count_lines(out);
        constexpr int kGutterMinLines = 5;
        if (line_count >= kGutterMinLines) {
            // Width of the line-number column. log10-style.
            int w_digits = 1;
            for (int v = line_count; v >= 10; v /= 10) ++w_digits;
            constexpr std::string_view kSep =
                " \xe2\x94\x82 ";  // " │ " (U+2502 = ~3 cells wide visually with pad)
            const std::size_t kSepBytes = kSep.size();
            const Style& gstyle = syntax::gutter();

            // Pre-split runs at newline boundaries so no run spans
            // a line break — block comments in C-style languages emit
            // a single run for the whole comment, including embedded
            // \n. Splitting up-front lets the line-shift remap below
            // be a simple per-line offset add rather than a byte-by-
            // byte run split-walk.
            std::vector<StyledRun> runs_split;
            runs_split.reserve(runs.size() * 2);
            for (const auto& r : runs) {
                std::size_t cur = r.byte_offset;
                const std::size_t end = cur + r.byte_length;
                while (cur < end) {
                    std::size_t nl = out.find('\n', cur);
                    std::size_t seg_end = (nl == std::string::npos || nl >= end)
                                        ? end : nl;
                    if (seg_end > cur) {
                        runs_split.push_back({cur, seg_end - cur, r.style});
                    }
                    if (nl != std::string::npos && nl < end) {
                        runs_split.push_back({nl, 1, r.style});
                        cur = nl + 1;
                    } else {
                        break;
                    }
                }
            }

            // Build line-start offset table for the original `out`.
            std::vector<std::size_t> line_starts;
            line_starts.reserve(static_cast<std::size_t>(line_count) + 1);
            line_starts.push_back(0);
            for (std::size_t k = 0; k < out.size(); ++k) {
                if (out[k] == '\n') line_starts.push_back(k + 1);
            }
            // Sentinel for the binary-search remap below: any run
            // offset is < out.size() < line_starts.back() + ε.

            // Emit out2 with per-line gutter prefixes.
            std::string out2;
            out2.reserve(out.size() + line_starts.size()
                         * (static_cast<std::size_t>(w_digits) + kSepBytes));
            std::vector<StyledRun> runs2;
            runs2.reserve(runs_split.size() + line_starts.size() * 2);

            for (std::size_t i = 0; i < line_starts.size(); ++i) {
                // Gutter for line i+1.
                char buf[24];
                int n = std::snprintf(buf, sizeof(buf), "%*zu",
                                      w_digits, i + 1);
                runs2.push_back({out2.size(), static_cast<std::size_t>(n), gstyle});
                out2.append(buf, static_cast<std::size_t>(n));
                runs2.push_back({out2.size(), kSepBytes, gstyle});
                out2.append(kSep);

                // Line content.
                std::size_t s = line_starts[i];
                std::size_t e = (i + 1 < line_starts.size())
                              ? line_starts[i + 1]
                              : out.size();
                out2.append(out.data() + s, e - s);
            }

            // Remap each split run.
            const std::size_t per_line_shift =
                static_cast<std::size_t>(w_digits) + kSepBytes;
            for (const auto& r : runs_split) {
                // Find which line this run sits on. line_starts is
                // sorted; r.byte_offset is in [line_starts[i],
                // line_starts[i+1]) for the line index i.
                auto it = std::upper_bound(
                    line_starts.begin(), line_starts.end(), r.byte_offset);
                std::size_t line_idx = static_cast<std::size_t>(
                    (it - line_starts.begin())) - 1;
                std::size_t shift = (line_idx + 1) * per_line_shift;
                runs2.push_back({r.byte_offset + shift,
                                 r.byte_length, r.style});
            }

            // Sort by offset for traversal-friendly downstream use.
            std::sort(runs2.begin(), runs2.end(),
                [](const StyledRun& a, const StyledRun& b) {
                    return a.byte_offset < b.byte_offset;
                });

            out = std::move(out2);
            runs = std::move(runs2);
        }
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = syntax::plain(),
        // Code blocks must not soft-wrap: a long identifier or
        // command line is one logical unit, and breaking it at a
        // column boundary spills continuation bytes back to column
        // 0 — directly under (or through) the line-number gutter.
        // Visible symptom: the next line’s gutter digit lands
        // *inside* the wrapped tail of the previous line. Hand the
        // overflow to the parent box’s clip rect instead; the
        // CodeBlock builder in render.cpp opts into
        // Overflow::Hidden so the tail simply truncates at the
        // right border.
        .wrap = TextWrap::NoWrap,
        .runs = std::move(runs),
    }};
}

// ============================================================================
// highlight_code — memoising wrapper
// ============================================================================
// The highlighter is pure: same `(lang_tag, code)` produces the same
// Element every call. For long agent transcripts the same code block is
// re-rendered tens or hundreds of times — once per inline frame — so a
// content-keyed cache turns the recurring cost into a hash lookup.
//
// Cache is thread_local (renderer is single-threaded per app instance
// in practice; this avoids contention if the host ever spawns a side
// renderer). FIFO-evicts in halves when capacity is exceeded so we
// never grow unbounded across long sessions. Keying combines a 64-bit
// FNV-1a hash of `code` with a hash of `lang_tag`; the cached Element
// is returned by copy, which for the typical TextElement case is a
// string + runs vector — cheap relative to running the highlighter.
static Element highlight_code(const std::string& code, const std::string& lang_tag) {
    struct CacheEntry {
        uint64_t key;
        Element  elem;
    };
    static constexpr std::size_t kMaxEntries = 256;
    thread_local std::vector<CacheEntry> cache;

    auto fnv1a = [](std::string_view sv, uint64_t seed) noexcept -> uint64_t {
        uint64_t h = seed;
        for (unsigned char c : sv) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    };
    const uint64_t key = fnv1a(code, fnv1a(lang_tag, 14695981039346656037ULL));

    for (auto& e : cache) {
        if (e.key == key) return e.elem;
    }

    Element elem = highlight_code_impl(code, lang_tag);

    if (cache.size() >= kMaxEntries) {
        cache.erase(cache.begin(), cache.begin() + kMaxEntries / 2);
    }
    cache.push_back({key, elem});
    return elem;
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
Element highlight_code(const std::string& code, const std::string& lang_tag) {
    return ::maya::highlight_code(code, lang_tag);
}
} // namespace md_detail

} // namespace maya
