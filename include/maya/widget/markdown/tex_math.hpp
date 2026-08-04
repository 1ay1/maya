#pragma once
// tex_math.hpp — a state-of-the-art LaTeX→terminal math typesetter.
//
// This is the "KaTeX for a cell grid" that powers maya's math markdown
// extension. It is provider-agnostic by construction: Claude, OpenAI, Gemini
// and every other model emit the SAME TeX source ($…$, $$…$$, \(…\), \[…\],
// or ```math fences), so a renderer that understands TeX understands them all.
//
// ── Why a 2D box model (and not string substitution) ────────────────────────
// Naive terminal math renderers flatten everything to one line: `a/b`, `x^2`,
// `sqrt(x)`. That falls apart the moment real math shows up — a fraction, a
// summation with limits, a matrix, a nested radical. maya instead ports TeX's
// box-and-glue idea to a character grid:
//
//   • Every sub-expression typesets into a `Box`: a rectangular grid of
//     styled cells plus an `axis` — the row index that sits on the math
//     baseline. Ascent = axis, descent = rows - axis - 1.
//   • Boxes compose. Horizontal concatenation aligns two boxes on their
//     shared axis (padding the shorter one above/below). A fraction stacks
//     numerator / rule / denominator and puts the axis ON the rule. A
//     superscript raises a box and shrinks the parent's implied axis; a
//     subscript lowers it. Roots draw a radical that grows with its radicand.
//
// The result: `\frac{-b\pm\sqrt{b^2-4ac}}{2a}` renders as a real three-row
// fraction with a real radical, in monochrome-safe Unicode, at any width.
//
// ── Scope ───────────────────────────────────────────────────────────────────
// Header-only, no deps beyond maya's Element/Style. Covers the LaTeX subset
// that actually appears in chat-model output: Greek, operators, relations,
// arrows, set/logic symbols, blackboard/cal letters, \frac \dfrac \tfrac,
// \sqrt (incl. \sqrt[n]), ^ _ groups, \sum \int \prod \lim with limits,
// \left…\right auto-sizing delimiters, \begin{matrix|pmatrix|bmatrix|cases},
// \text{…}, spacing, accents (\hat \bar \vec …), and ~200 named symbols.
// Unknown macros degrade gracefully to their name rather than aborting.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/types.hpp"
#include "../../style/style.hpp"

namespace maya::texmath {

// ── Palette hooks ────────────────────────────────────────────────────────────
// The renderer resolves colors lazily so it stays decoupled from the markdown
// palette. Callers may override; defaults are terminal-safe.
struct MathPalette {
    Style normal;                         // variables / plain glyphs
    Style op       = Style{}.with_bold(); // operators + big operators
    Style num;                            // numbers
    Style rule;                           // fraction bars, radical strokes
    Style delim;                          // parens / brackets / braces
    Style text;                           // \text{…} escape hatch
};

// One typeset cell: a UTF-8 grapheme (1–4 bytes) and its style.
struct Cell {
    std::array<char, 4> bytes{};
    std::uint8_t        len = 0;
    Style               style;
    std::uint8_t        width = 1;  // display columns (2 for wide glyphs)
};

// A rectangular grid of cells plus the baseline (axis) row. Empty cells are
// rendered as spaces. This is the universal currency of the layout engine.
struct Box {
    int               rows = 1;
    int               cols = 0;
    int               axis = 0;               // baseline row (0-based)
    std::vector<Cell> grid;                   // rows*cols, row-major

    [[nodiscard]] int ascent()  const noexcept { return axis; }
    [[nodiscard]] int descent() const noexcept { return rows - axis - 1; }

    Cell&       at(int r, int c)       noexcept { return grid[static_cast<std::size_t>(r) * cols + c]; }
    const Cell& at(int r, int c) const noexcept { return grid[static_cast<std::size_t>(r) * cols + c]; }
};

// ── Public API ───────────────────────────────────────────────────────────────
// Typeset a LaTeX math string into a Box. `display=false` uses inline
// conventions (\textstyle: limits go to sub/superscript position, smaller
// fraction bars); `display=true` uses \displaystyle (limits above/below).
[[nodiscard]] Box typeset(std::string_view latex, const MathPalette& pal,
                          bool display);

// (render_math — typeset + flatten into a maya Element — lives in the sibling
// header tex_math_render.hpp, which pulls in the Element/builder machinery.
// This header stays dependency-light so the typesetter can be unit-tested
// against a plain cell grid.)

// ─────────────────────────────────────────────────────────────────────────────
//                              IMPLEMENTATION
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

inline Cell make_cell(std::string_view g, Style s, int w = 1) {
    Cell c;
    c.len = static_cast<std::uint8_t>(g.size() > 4 ? 4 : g.size());
    for (std::uint8_t i = 0; i < c.len; ++i) c.bytes[i] = g[i];
    c.style = s;
    c.width = static_cast<std::uint8_t>(w < 1 ? 1 : w);
    return c;
}

// Blank box of a given shape (spaces on the given axis).
inline Box blank(int rows, int cols, int axis, Style s) {
    Box b;
    b.rows = rows < 1 ? 1 : rows;
    b.cols = cols < 0 ? 0 : cols;
    b.axis = axis < 0 ? 0 : (axis >= b.rows ? b.rows - 1 : axis);
    b.grid.assign(static_cast<std::size_t>(b.rows) * b.cols, make_cell(" ", s));
    return b;
}

// A single-row box from a UTF-8 string. Each grapheme is one cell; a small
// combining/width heuristic keeps CJK/emoji at width 2 and drops combining
// marks onto the previous cell.
inline Box hbox(std::string_view s, Style style) {
    std::vector<Cell> cells;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c0 >= 0xF0) len = 4;
        else if (c0 >= 0xE0) len = 3;
        else if (c0 >= 0xC0) len = 2;
        if (i + len > s.size()) len = 1;
        std::string_view g = s.substr(i, len);
        // Wide: CJK (U+3000..U+9FFF, U+FF00..), rough gate on lead bytes.
        int w = 1;
        if (len == 3) {
            char32_t cp = (static_cast<char32_t>(c0 & 0x0F) << 12) |
                          (static_cast<char32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<char32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F));
            if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                (cp >= 0xFFE0 && cp <= 0xFFE6))
                w = 2;
        }
        cells.push_back(make_cell(g, style, w));
        i += len;
    }
    Box b;
    b.rows = 1;
    b.axis = 0;
    b.cols = 0;
    for (auto& c : cells) b.cols += c.width;
    // Re-expand wide cells: a width-2 cell occupies 2 grid columns (2nd blank).
    b.grid.reserve(static_cast<std::size_t>(b.cols));
    for (auto& c : cells) {
        b.grid.push_back(c);
        if (c.width == 2) b.grid.push_back(make_cell("", c.style, 1)); // continuation
    }
    if (b.cols == 0) { b.cols = 0; }
    return b;
}

// Stack `a` left of `b`, aligning on the shared axis. The joined axis is the
// max of the two ascents; rows below is the max of the two descents.
inline Box hcat(const Box& a, const Box& b) {
    if (a.cols == 0) return b;
    if (b.cols == 0) return a;
    int asc = std::max(a.ascent(), b.ascent());
    int desc = std::max(a.descent(), b.descent());
    int rows = asc + desc + 1;
    Box out = blank(rows, a.cols + b.cols, asc, Style{});
    auto blit = [&](const Box& src, int col0) {
        int top = asc - src.ascent();
        for (int r = 0; r < src.rows; ++r)
            for (int c = 0; c < src.cols; ++c)
                out.at(top + r, col0 + c) = src.at(r, c);
    };
    blit(a, 0);
    blit(b, a.cols);
    return out;
}

inline Box hcat3(const Box& a, const Box& b, const Box& c) {
    return hcat(hcat(a, b), c);
}

// Vertically stack boxes (top→bottom), centering narrower ones, and place the
// axis on row `axis_row`. Used by fractions, matrices, big operators.
inline Box vstack(const std::vector<Box>& parts, int axis_row, Style fill) {
    int cols = 0, rows = 0;
    for (auto& p : parts) { cols = std::max(cols, p.cols); rows += p.rows; }
    Box out = blank(rows, cols, axis_row, fill);
    int r0 = 0;
    for (auto& p : parts) {
        int c0 = (cols - p.cols) / 2;
        for (int r = 0; r < p.rows; ++r)
            for (int c = 0; c < p.cols; ++c)
                out.at(r0 + r, c0 + c) = p.at(r, c);
        r0 += p.rows;
    }
    return out;
}

// ── Named-symbol table ───────────────────────────────────────────────────────
// Maps a LaTeX control word (without the backslash) to its Unicode glyph.
// Covers the set that actually shows up in model output. Lookup is a linear
// scan over a constexpr sorted-ish array; the table is small and only touched
// once per macro during parse, so a hash map isn't worth the header weight.
struct SymEntry { std::string_view name; std::string_view glyph; };

inline constexpr SymEntry kSymbols[] = {
    // lowercase Greek
    {"alpha","\u03b1"},{"beta","\u03b2"},{"gamma","\u03b3"},{"delta","\u03b4"},
    {"epsilon","\u03b5"},{"varepsilon","\u03b5"},{"zeta","\u03b6"},{"eta","\u03b7"},
    {"theta","\u03b8"},{"vartheta","\u03d1"},{"iota","\u03b9"},{"kappa","\u03ba"},
    {"lambda","\u03bb"},{"mu","\u03bc"},{"nu","\u03bd"},{"xi","\u03be"},
    {"pi","\u03c0"},{"varpi","\u03d6"},{"rho","\u03c1"},{"varrho","\u03f1"},
    {"sigma","\u03c3"},{"varsigma","\u03c2"},{"tau","\u03c4"},{"upsilon","\u03c5"},
    {"phi","\u03c6"},{"varphi","\u03d5"},{"chi","\u03c7"},{"psi","\u03c8"},
    {"omega","\u03c9"},
    // uppercase Greek
    {"Gamma","\u0393"},{"Delta","\u0394"},{"Theta","\u0398"},{"Lambda","\u039b"},
    {"Xi","\u039e"},{"Pi","\u03a0"},{"Sigma","\u03a3"},{"Upsilon","\u03a5"},
    {"Phi","\u03a6"},{"Psi","\u03a8"},{"Omega","\u03a9"},
    // binary operators
    {"times","\u00d7"},{"div","\u00f7"},{"pm","\u00b1"},{"mp","\u2213"},
    {"cdot","\u00b7"},{"ast","\u2217"},{"star","\u22c6"},{"circ","\u2218"},
    {"bullet","\u2219"},{"oplus","\u2295"},{"ominus","\u2296"},{"otimes","\u2297"},
    {"oslash","\u2298"},{"odot","\u2299"},{"cap","\u2229"},{"cup","\u222a"},
    {"sqcap","\u2293"},{"sqcup","\u2294"},{"wedge","\u2227"},{"land","\u2227"},
    {"vee","\u2228"},{"lor","\u2228"},{"setminus","\u2216"},{"wr","\u2240"},
    {"amalg","\u2a3f"},{"dagger","\u2020"},{"ddagger","\u2021"},
    // relations
    {"leq","\u2264"},{"le","\u2264"},{"geq","\u2265"},{"ge","\u2265"},
    {"neq","\u2260"},{"ne","\u2260"},{"equiv","\u2261"},{"approx","\u2248"},
    {"cong","\u2245"},{"simeq","\u2243"},{"sim","\u223c"},{"propto","\u221d"},
    {"ll","\u226a"},{"gg","\u226b"},{"subset","\u2282"},{"supset","\u2283"},
    {"subseteq","\u2286"},{"supseteq","\u2287"},{"sqsubseteq","\u2291"},
    {"sqsupseteq","\u2292"},{"in","\u2208"},{"ni","\u220b"},{"notin","\u2209"},
    {"vdash","\u22a2"},{"dashv","\u22a3"},{"models","\u22a8"},{"perp","\u22a5"},
    {"mid","\u2223"},{"parallel","\u2225"},{"asymp","\u224d"},{"doteq","\u2250"},
    {"prec","\u227a"},{"succ","\u227b"},{"preceq","\u2aaf"},{"succeq","\u2ab0"},
    // arrows
    {"leftarrow","\u2190"},{"gets","\u2190"},{"rightarrow","\u2192"},{"to","\u2192"},
    {"leftrightarrow","\u2194"},{"Leftarrow","\u21d0"},{"Rightarrow","\u21d2"},
    {"implies","\u27f9"},{"impliedby","\u27f8"},{"iff","\u27fa"},
    {"Leftrightarrow","\u21d4"},{"mapsto","\u21a6"},{"uparrow","\u2191"},
    {"downarrow","\u2193"},{"updownarrow","\u2195"},{"nearrow","\u2197"},
    {"searrow","\u2198"},{"swarrow","\u2199"},{"nwarrow","\u2196"},
    {"longrightarrow","\u27f6"},{"longleftarrow","\u27f5"},{"hookrightarrow","\u21aa"},
    {"hookleftarrow","\u21a9"},
    // misc symbols
    {"infty","\u221e"},{"partial","\u2202"},{"nabla","\u2207"},{"forall","\u2200"},
    {"exists","\u2203"},{"nexists","\u2204"},{"emptyset","\u2205"},{"varnothing","\u2205"},
    {"neg","\u00ac"},{"lnot","\u00ac"},{"top","\u22a4"},{"bot","\u22a5"},
    {"angle","\u2220"},{"triangle","\u25b3"},{"square","\u25a1"},{"Box","\u25a1"},
    {"diamond","\u22c4"},{"Diamond","\u25c7"},{"aleph","\u2135"},{"hbar","\u210f"},
    {"ell","\u2113"},{"Re","\u211c"},{"Im","\u2111"},{"wp","\u2118"},
    {"prime","\u2032"},{"degree","\u00b0"},{"deg","\u00b0"},{"surd","\u221a"},
    {"flat","\u266d"},{"sharp","\u266f"},{"natural","\u266e"},{"clubsuit","\u2663"},
    {"diamondsuit","\u2662"},{"heartsuit","\u2661"},{"spadesuit","\u2660"},
    {"dots","\u2026"},{"ldots","\u2026"},{"cdots","\u22ef"},{"vdots","\u22ee"},
    {"ddots","\u22f1"},{"therefore","\u2234"},{"because","\u2235"},{"checkmark","\u2713"},
    {"complement","\u2201"},{"backslash","\\"},
    // blackboard-bold shortcuts for common sets
    {"mathbb{R}","\u211d"},{"R","\u211d"},{"mathbb{N}","\u2115"},{"N","\u2115"},
    {"mathbb{Z}","\u2124"},{"Z","\u2124"},{"mathbb{Q}","\u211a"},{"Q","\u211a"},
    {"mathbb{C}","\u2102"},{"C","\u2102"},{"mathbb{P}","\u2119"},{"mathbb{H}","\u210d"},
    // named functions render as themselves (upright) — handled by parser, but
    // a few common ones alias here for completeness of the fallback path.
    {"quad","  "},{"qquad","    "},{","," "},{":"," "},{";"," "},{"!",""},
    {" "," "},{"lbrace","{"},{"rbrace","}"},{"langle","\u27e8"},{"rangle","\u27e9"},
    {"lceil","\u2308"},{"rceil","\u2309"},{"lfloor","\u230a"},{"rfloor","\u230b"},
};

inline std::string_view lookup_symbol(std::string_view name) {
    for (const auto& e : kSymbols) if (e.name == name) return e.glyph;
    return {};
}

// Upright multi-letter operator names (\sin, \log, …). These render as roman
// text, unlike single-letter variables which render italic-by-convention.
inline bool is_named_op(std::string_view w) {
    static constexpr std::string_view kOps[] = {
        "sin","cos","tan","cot","sec","csc","sinh","cosh","tanh","coth",
        "arcsin","arccos","arctan","log","ln","lg","exp","det","dim","ker",
        "deg","gcd","hom","arg","max","min","sup","inf","lim","limsup",
        "liminf","mod","bmod","Pr","tr","rank",
    };
    for (auto o : kOps) if (o == w) return true;
    return false;
}

// Unicode sub/superscript forms for digits, +, -, =, ( ), and a few letters —
// used for inline (\textstyle) exponents/indices that fit on one row so we
// don't force a 3-row box for `x^2`.
inline std::string_view sup_glyph(char c) {
    switch (c) {
        case '0': return "\u2070"; case '1': return "\u00b9"; case '2': return "\u00b2";
        case '3': return "\u00b3"; case '4': return "\u2074"; case '5': return "\u2075";
        case '6': return "\u2076"; case '7': return "\u2077"; case '8': return "\u2078";
        case '9': return "\u2079"; case '+': return "\u207a"; case '-': return "\u207b";
        case '=': return "\u207c"; case '(': return "\u207d"; case ')': return "\u207e";
        case 'n': return "\u207f"; case 'i': return "\u2071";
        default:  return {};
    }
}
inline std::string_view sub_glyph(char c) {
    switch (c) {
        case '0': return "\u2080"; case '1': return "\u2081"; case '2': return "\u2082";
        case '3': return "\u2083"; case '4': return "\u2084"; case '5': return "\u2085";
        case '6': return "\u2086"; case '7': return "\u2087"; case '8': return "\u2088";
        case '9': return "\u2089"; case '+': return "\u208a"; case '-': return "\u208b";
        case '=': return "\u208c"; case '(': return "\u208d"; case ')': return "\u208e";
        case 'a': return "\u2090"; case 'e': return "\u2091"; case 'i': return "\u1d62";
        case 'o': return "\u2092"; case 'x': return "\u2093"; case 'j': return "\u2c7c";
        default:  return {};
    }
}

// Combining accents for \hat \bar \vec \dot \tilde etc. (applied to the last
// cell of the accented box).
inline std::string_view accent_combining(std::string_view name) {
    if (name == "hat"  || name == "widehat")   return "\u0302";
    if (name == "bar"  || name == "overline")  return "\u0304";
    if (name == "vec")                          return "\u20d7";
    if (name == "dot")                          return "\u0307";
    if (name == "ddot")                         return "\u0308";
    if (name == "tilde"|| name == "widetilde") return "\u0303";
    if (name == "check")                        return "\u030c";
    if (name == "acute")                        return "\u0301";
    if (name == "grave")                        return "\u0300";
    if (name == "breve")                        return "\u0306";
    return {};
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  Parser / typesetter
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

struct Parser {
    std::string_view s;
    std::size_t      i = 0;
    const MathPalette& pal;
    bool             display;

    Parser(std::string_view src, const MathPalette& p, bool disp)
        : s(src), pal(p), display(disp) {}

    bool eof() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    void skip_ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i; }

    // Read a control word: backslash already consumed. Returns the name; a
    // control SYMBOL (\{, \,, \|) returns a 1-char name.
    std::string_view read_ctrl() {
        std::size_t start = i;
        if (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) {
            ++i;
            return s.substr(start, 1);
        }
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(start, i - start);
    }

    // Parse a {...}-group or a single token into a Box. Used by \frac args,
    // ^ _ operands, \sqrt radicand, accents.
    Box parse_group() {
        skip_ws();
        if (peek() == '{') {
            ++i;
            Box b = parse_seq('}');
            if (peek() == '}') ++i;
            return b;
        }
        // single token
        return parse_atom();
    }

    // Parse a sequence of atoms until `stop` (or eof), concatenating boxes.
    Box parse_seq(char stop) {
        Box acc;  // empty
        while (!eof() && peek() != stop) {
            // \right / \end terminate a sub-parse regardless of `stop`.
            if (peek() == '\\') {
                std::size_t save = i;
                ++i;
                std::string_view w = read_ctrl();
                if (w == "right" || w == "end" || w == "\\") { i = save; break; }
                i = save;
            }
            if (peek() == '&') break;  // matrix column sep
            Box a = parse_atom();
            a = maybe_scripts(std::move(a));
            acc = hcat(acc, a);
        }
        return acc;
    }

    // After an atom, attach ^ and _ scripts if present.
    Box maybe_scripts(Box base) {
        Box sup, sub;
        bool has_sup = false, has_sub = false;
        for (;;) {
            skip_ws();
            if (peek() == '^') { ++i; sup = parse_group(); has_sup = true; }
            else if (peek() == '_') { ++i; sub = parse_group(); has_sub = true; }
            else break;
        }
        if (!has_sup && !has_sub) return base;
        return attach_scripts(std::move(base), sup, sub, has_sup, has_sub);
    }

    // Compose a base with raised superscript and lowered subscript.
    Box attach_scripts(Box base, Box sup, Box sub, bool has_sup, bool has_sub) {
        // Fast path: single-row single-cell scripts fold into Unicode
        // sub/superscript glyphs (x^2 → x²) so simple exponents stay 1 row.
        auto try_unicode = [](const Box& b, bool super) -> std::string {
            if (b.rows != 1) return {};
            std::string out;
            for (int c = 0; c < b.cols; ++c) {
                const Cell& cell = b.at(0, c);
                if (cell.len == 0) continue;
                if (cell.len != 1) return {};
                std::string_view g = super ? sup_glyph(cell.bytes[0])
                                           : sub_glyph(cell.bytes[0]);
                if (g.empty()) return {};
                out += g;
            }
            return out;
        };
        if (has_sup && !has_sub) {
            std::string u = try_unicode(sup, true);
            if (!u.empty()) return hcat(base, hbox(u, pal.normal));
        }
        if (has_sub && !has_sup) {
            std::string u = try_unicode(sub, false);
            if (!u.empty()) return hcat(base, hbox(u, pal.normal));
        }
        // General path: build a 2-or-3 row box beside the base. Superscript sits
        // above the axis, subscript below.
        int width = std::max(has_sup ? sup.cols : 0, has_sub ? sub.cols : 0);
        std::vector<Box> parts;
        int axis_row;
        if (has_sup && has_sub) {
            parts = {sup, blank(1, width, 0, pal.normal), sub};
            axis_row = sup.rows;  // middle spacer row
        } else if (has_sup) {
            parts = {sup, blank(1, width, 0, pal.normal)};
            axis_row = sup.rows;
        } else {
            parts = {blank(1, width, 0, pal.normal), sub};
            axis_row = 0;
        }
        Box scripts = vstack(parts, axis_row, pal.normal);
        return hcat(base, scripts);
    }

    // ── one atom ──────────────────────────────────────────────────────────
    Box parse_atom() {
        skip_ws();
        if (eof()) return Box{};
        char c = peek();
        if (c == '\\') { ++i; return parse_ctrl(); }
        if (c == '{')  { return parse_group(); }
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '|') {
            ++i; return hbox(std::string_view(&s[i - 1], 1), pal.delim);
        }
        // digit run
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = i;
            while (i < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
                ++i;
            return hbox(s.substr(start, i - start), pal.num);
        }
        // operators / relations
        if (c=='+'||c=='-'||c=='='||c=='<'||c=='>'||c=='*'||c=='/') {
            ++i;
            // render around a bit of air for readability
            return hbox(std::string_view(&s[i - 1], 1), pal.op);
        }
        // a letter → italic variable (single char)
        if (std::isalpha(static_cast<unsigned char>(c))) {
            ++i;
            return hbox(std::string_view(&s[i - 1], 1),
                        pal.normal.with_italic());
        }
        // anything else: literal
        ++i;
        return hbox(std::string_view(&s[i - 1], 1), pal.normal);
    }

    // ── control sequences ────────────────────────────────────────────────
    Box parse_ctrl() {
        std::string_view w = read_ctrl();

        if (w == "frac" || w == "dfrac" || w == "tfrac" || w == "binom") {
            Box top = parse_group();
            Box bot = parse_group();
            if (w == "binom") return make_binom(top, bot);
            return make_frac(top, bot);
        }
        if (w == "sqrt") {
            Box index;
            bool has_index = false;
            skip_ws();
            if (peek() == '[') {
                ++i; index = parse_seq(']'); if (peek() == ']') ++i; has_index = true;
            }
            Box rad = parse_group();
            return make_sqrt(rad, index, has_index);
        }
        if (w == "left") {
            char open = eat_delim();
            Box inner = parse_seq('\0');  // until \right
            char close = '\0';
            if (peek() == '\\') {
                std::size_t save = i; ++i;
                if (read_ctrl() == "right") close = eat_delim(); else i = save;
            }
            return wrap_delims(inner, open, close);
        }
        if (w == "text" || w == "mathrm" || w == "operatorname" ||
            w == "mathbf" || w == "mathit" || w == "mathsf" || w == "mathtt") {
            skip_ws();
            if (peek() == '{') {
                ++i;
                std::size_t start = i;
                int depth = 1;
                while (i < s.size() && depth) {
                    if (s[i] == '{') ++depth;
                    else if (s[i] == '}') { if (--depth == 0) break; }
                    ++i;
                }
                std::string_view body = s.substr(start, i - start);
                if (peek() == '}') ++i;
                Style st = (w == "mathbf") ? pal.text.with_bold()
                         : (w == "mathit") ? pal.text.with_italic()
                         : pal.text;
                return hbox(body, st);
            }
            return Box{};
        }
        if (w == "begin") return parse_env();

        // accents
        if (!accent_combining(w).empty()) {
            Box b = parse_group();
            apply_accent(b, accent_combining(w));
            return b;
        }

        // big operators with limits (\sum \int \prod \lim \bigcup …)
        if (auto big = big_operator(w); !big.empty())
            return parse_bigop(big, w);

        // named function (\sin \log …): upright, keep following space
        if (is_named_op(w)) return hbox(w, pal.op);

        // \mathbb{X} etc. — try the combined key first (e.g. "mathbb{R}").
        if (w == "mathbb" || w == "mathcal" || w == "mathfrak") {
            skip_ws();
            if (peek() == '{') {
                std::size_t save = i; ++i;
                std::size_t start = i;
                while (i < s.size() && s[i] != '}') ++i;
                std::string_view inner = s.substr(start, i - start);
                if (peek() == '}') ++i;
                std::string combined = std::string(w) + "{" + std::string(inner) + "}";
                if (auto g = lookup_symbol(combined); !g.empty())
                    return hbox(g, pal.normal);
                // fallback: render the letters upright/bold
                i = save;
                Box b = parse_group();
                return b;
            }
        }

        // named symbol
        if (auto g = lookup_symbol(w); !g.empty()) {
            bool opish = (w=="sum"||w=="int"||w=="prod"); // handled above, safety
            Style st = opish ? pal.op : pal.normal;
            return hbox(g, st);
        }

        // spacing commands already in table as " "; unknown → dim literal name
        return hbox(std::string("\\") + std::string(w),
                    pal.text.with_dim());
    }

    // Big-operator glyph for a control word, or empty.
    static std::string_view big_operator(std::string_view w) {
        if (w == "sum")      return "\u2211";
        if (w == "prod")     return "\u220f";
        if (w == "coprod")   return "\u2210";
        if (w == "int")      return "\u222b";
        if (w == "iint")     return "\u222c";
        if (w == "iiint")    return "\u222d";
        if (w == "oint")     return "\u222e";
        if (w == "bigcup")   return "\u22c3";
        if (w == "bigcap")   return "\u22c2";
        if (w == "bigvee")   return "\u22c1";
        if (w == "bigwedge") return "\u22c0";
        if (w == "bigoplus") return "\u2a01";
        if (w == "bigotimes")return "\u2a02";
        if (w == "lim")      return "lim";
        return {};
    }

    // \sum_{i=0}^{n}: in display mode put limits above/below, else as scripts.
    Box parse_bigop(std::string_view glyph, std::string_view name) {
        Box lower, upper;
        bool has_lo = false, has_up = false;
        for (;;) {
            skip_ws();
            if (peek() == '_') { ++i; lower = parse_group(); has_lo = true; }
            else if (peek() == '^') { ++i; upper = parse_group(); has_up = true; }
            else break;
        }
        Box op = hbox(glyph, pal.op.with_bold());
        bool limits_above = display && name != "int" && name != "iint" &&
                            name != "iiint" && name != "oint";
        if (!has_lo && !has_up) return op;
        if (limits_above) {
            std::vector<Box> parts;
            int axis_row = 0;
            if (has_up) { parts.push_back(upper); ++axis_row; }
            parts.push_back(op);
            if (has_lo) parts.push_back(lower);
            return vstack(parts, axis_row, pal.normal);
        }
        return attach_scripts(op, upper, lower, has_up, has_lo);
    }

    // ── environments: matrix / pmatrix / bmatrix / cases ────────────────────
    Box parse_env() {
        skip_ws();
        std::string kind;
        if (peek() == '{') {
            ++i;
            std::size_t start = i;
            while (i < s.size() && s[i] != '}') ++i;
            kind = std::string(s.substr(start, i - start));
            if (peek() == '}') ++i;
        }
        // collect rows of cells
        std::vector<std::vector<Box>> matrix;
        matrix.emplace_back();
        for (;;) {
            skip_ws();
            if (eof()) break;
            if (peek() == '\\') {
                std::size_t save = i; ++i;
                std::string_view w = read_ctrl();
                if (w == "end") {
                    skip_ws();
                    if (peek() == '{') { while (i < s.size() && s[i] != '}') ++i; if (peek()=='}') ++i; }
                    break;
                }
                if (w == "\\") { matrix.emplace_back(); continue; }  // row break
                i = save;
            }
            if (peek() == '&') { ++i; matrix.back().emplace_back(); continue; }
            Box cell = parse_seq_cell();
            if (matrix.back().empty()) matrix.back().push_back(cell);
            else matrix.back().back() = hcat(matrix.back().back(), cell);
        }
        return assemble_matrix(matrix, kind);
    }

    // parse one matrix cell: until & , \\ , or \end
    Box parse_seq_cell() {
        Box acc;
        while (!eof()) {
            skip_ws();
            if (peek() == '&') break;
            if (peek() == '\\') {
                std::size_t save = i; ++i;
                std::string_view w = read_ctrl();
                i = save;
                if (w == "\\" || w == "end") break;
            }
            Box a = parse_atom();
            a = maybe_scripts(std::move(a));
            acc = hcat(acc, a);
        }
        return acc;
    }

    char eat_delim() {
        skip_ws();
        if (peek() == '\\') { ++i; std::string_view w = read_ctrl();
            if (w == ".") return '\0';
            if (w == "{") return '{'; if (w == "}") return '}';
            if (w == "langle") return '<'; if (w == "rangle") return '>';
            return '\0';
        }
        char c = peek();
        if (c) ++i;
        if (c == '.') return '\0';
        return c;
    }

    // ── constructors that build multi-row boxes ────────────────────────────
    Box make_frac(const Box& num, const Box& den) {
        int w = std::max(num.cols, den.cols);
        w = std::max(w, 1);
        Box bar = blank(1, w, 0, pal.rule);
        for (int c = 0; c < w; ++c) bar.at(0, c) = make_cell("\u2500", pal.rule);
        Box stacked = vstack({num, bar, den}, num.rows, pal.normal);
        // axis on the bar row
        stacked.axis = num.rows;
        return stacked;
    }

    Box make_binom(const Box& top, const Box& bot) {
        Box stacked = vstack({top, bot}, top.rows - 1 + (bot.rows>0?0:0), pal.normal);
        stacked.axis = (stacked.rows - 1) / 2;
        return wrap_delims(stacked, '(', ')');
    }

    Box make_sqrt(const Box& rad, const Box& index, bool has_index) {
        int h = rad.rows;
        int w = rad.cols;
        // Radical layout:
        //      ___________          row 0: vinculum spanning the radicand,
        //   \ /  radicand           joined to the stroke's top-right nook
        //    v                      rows 1..: stroke col + radicand
        //
        // The stroke occupies column 0; a one-column gap (column 1) separates
        // it from the radicand so the '√' hook doesn't collide with the first
        // glyph. The vinculum therefore starts at column 1 and runs to the
        // right edge, sitting flush above the whole radicand.
        int stroke_col = 0;
        int pad_col    = 1;   // gap between stroke and radicand
        int rad_col    = pad_col + 1;
        int extra_top  = 1;   // vinculum row
        int total_w    = rad_col + w;
        Box out = blank(h + extra_top, total_w, rad.axis + extra_top, pal.normal);
        // vinculum: from the gap column across the full radicand width
        for (int c = pad_col; c < total_w; ++c)
            out.at(0, c) = make_cell("\u203e", pal.rule);
        // radical stroke on the baseline row of the radicand region
        out.at(extra_top + std::max(0, h - 1), stroke_col) =
            make_cell("\u221a", pal.rule);
        // blit radicand
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c)
                out.at(extra_top + r, rad_col + c) = rad.at(r, c);
        if (has_index && index.cols > 0) {
            // small index tucked to the upper-left of the stroke (best effort)
            return hcat(index, out);
        }
        return out;
    }

    Box wrap_delims(const Box& inner, char open, char close) {
        int h = inner.rows;
        Box out = inner;
        auto tall = [&](char d, bool left) -> Box {
            if (h <= 1) {
                std::string_view g(&d, 1);
                std::string_view sub = (d=='\0') ? std::string_view(" ") : g;
                return hbox((d=='\0')?" ":sub, pal.delim);
            }
            // build a multi-row bracket
            Box col = blank(h, 1, inner.axis, pal.delim);
            std::string_view top, mid, bot, ext;
            switch (d) {
                case '(': top="\u239b"; ext="\u239c"; bot="\u239d"; mid="\u239c"; break;
                case ')': top="\u239e"; ext="\u239f"; bot="\u23a0"; mid="\u239f"; break;
                case '[': top="\u23a1"; ext="\u23a2"; bot="\u23a3"; mid="\u23a2"; break;
                case ']': top="\u23a4"; ext="\u23a5"; bot="\u23a6"; mid="\u23a5"; break;
                case '{': top="\u23a7"; ext="\u23a8"; bot="\u23a9"; mid="\u23aa"; break;
                case '}': top="\u23ab"; ext="\u23ac"; bot="\u23ad"; mid="\u23aa"; break;
                case '|': top="\u2502"; ext="\u2502"; bot="\u2502"; mid="\u2502"; break;
                default:  return blank(h, 1, inner.axis, pal.delim);
            }
            for (int r = 0; r < h; ++r) {
                std::string_view g = (r == 0) ? top : (r == h - 1) ? bot
                                    : (r == h/2) ? mid : ext;
                col.at(r, 0) = make_cell(g, pal.delim);
            }
            (void)left;
            return col;
        };
        if (open  != '\0') out = hcat(tall(open, true),  out);
        if (close != '\0') out = hcat(out, tall(close, false));
        return out;
    }

    Box assemble_matrix(const std::vector<std::vector<Box>>& m, const std::string& kind) {
        // drop trailing empty row
        std::vector<std::vector<Box>> rows = m;
        while (!rows.empty() && rows.back().empty()) rows.pop_back();
        if (rows.empty()) return Box{};
        std::size_t ncol = 0;
        for (auto& r : rows) ncol = std::max(ncol, r.size());
        std::vector<int> colw(ncol, 0);
        std::vector<int> rowh(rows.size(), 1), rowasc(rows.size(), 0);
        for (std::size_t r = 0; r < rows.size(); ++r)
            for (std::size_t c = 0; c < rows[r].size(); ++c) {
                colw[c]  = std::max(colw[c], rows[r][c].cols);
                rowh[r]  = std::max(rowh[r], rows[r][c].rows);
                rowasc[r]= std::max(rowasc[r], rows[r][c].ascent());
            }
        int gap = 1;
        int totalw = 0;
        for (auto w : colw) totalw += w;
        totalw += static_cast<int>(ncol > 0 ? (ncol - 1) * gap : 0);
        int totalh = 0;
        for (auto h : rowh) totalh += h;
        Box out = blank(totalh, totalw, totalh / 2, pal.normal);
        int r0 = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            int c0 = 0;
            for (std::size_t c = 0; c < ncol; ++c) {
                if (c < rows[r].size()) {
                    const Box& cell = rows[r][c];
                    int coff = c0 + (colw[c] - cell.cols) / 2;
                    int roff = r0 + (rowasc[r] - cell.ascent());
                    for (int rr = 0; rr < cell.rows; ++rr)
                        for (int cc = 0; cc < cell.cols; ++cc)
                            out.at(roff + rr, coff + cc) = cell.at(rr, cc);
                }
                c0 += colw[c] + gap;
            }
            r0 += rowh[r];
        }
        // brackets
        char open = '\0', close = '\0';
        if (kind == "pmatrix") { open='('; close=')'; }
        else if (kind == "bmatrix") { open='['; close=']'; }
        else if (kind == "Bmatrix") { open='{'; close='}'; }
        else if (kind == "vmatrix") { open='|'; close='|'; }
        else if (kind == "cases")   { open='{'; close='\0'; }
        if (open || close) out = wrap_delims(out, open, close);
        return out;
    }

    static void apply_accent(Box& b, std::string_view combining) {
        if (b.cols == 0 || b.rows == 0) return;
        // append the combining mark to the top-center cell of the top row.
        int c = b.cols / 2;
        Cell& cell = b.at(0, c);
        std::string merged;
        merged.assign(cell.bytes.data(), cell.len);
        merged += combining;
        cell = make_cell(merged, cell.style, cell.width);
    }
};

} // namespace detail

// ── public entry points ──────────────────────────────────────────────────────
inline Box typeset(std::string_view latex, const MathPalette& pal, bool display) {
    detail::Parser p(latex, pal, display);
    Box b = p.parse_seq('\0');
    if (b.cols == 0) b = detail::blank(1, 0, 0, pal.normal);
    return b;
}

// Linearize a typeset Box to a single UTF-8 string for INLINE math that must
// live inside a flowed paragraph run. The typeset already uses \textstyle
// (display=false), so most inline math is ALREADY one row (x^2, a_i,
// \alpha+\beta, \sin x) and passes straight through. The only common
// multi-row inline construct is a fraction, which collapses to `num/den`
// (the universal inline convention). Any other multi-row box (a stray inline
// matrix, nested radical) falls back to its baseline row so we never emit the
// scrambled row-interleaving a naive join would produce.
inline std::string linearize(std::string_view latex, const MathPalette& pal) {
    Box b = typeset(latex, pal, /*display=*/false);
    auto row_text = [&](int r) {
        std::string line;
        for (int c = 0; c < b.cols; ++c) {
            const Cell& cell = b.at(r, c);
            if (cell.len) line.append(cell.bytes.data(), cell.len);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        // also strip a leading run of spaces so centered rows align left
        std::size_t nb = line.find_first_not_of(' ');
        if (nb != std::string::npos && nb) line.erase(0, nb);
        return line;
    };
    if (b.rows == 1) return row_text(0);

    // Detect a simple fraction: exactly 3 rows whose middle (axis) row is a
    // solid run of the box-drawing bar '\u2500'. If so, join num/den with '/'.
    auto axis_all_bar = [&]() {
        bool any = false;
        for (int c = 0; c < b.cols; ++c) {
            const Cell& cell = b.at(1, c);
            if (cell.len == 0) continue;
            std::string_view g(cell.bytes.data(), cell.len);
            if (g == "\u2500") { any = true; continue; }
            if (g == " ") continue;
            return false;
        }
        return any;
    };
    if (b.rows == 3 && axis_all_bar()) {
        std::string num = row_text(0), den = row_text(2);
        if (!num.empty() && !den.empty()) {
            bool wrap_num = num.find_first_of("+- ") != std::string::npos;
            bool wrap_den = den.find_first_of("+- ") != std::string::npos;
            std::string out;
            out += wrap_num ? "(" + num + ")" : num;
            out += "/";
            out += wrap_den ? "(" + den + ")" : den;
            return out;
        }
    }
    // General multi-row inline (a fraction whose numerator/denominator is
    // itself tall, a nested radical, an inline matrix, or a fraction embedded
    // beside other terms like `x = \frac{…}{…}`): extracting a single row is
    // misleading — a fraction bar on the baseline would read as a stray `───`.
    // When the baseline row carries ANY bar glyph, return the raw LaTeX
    // (unambiguous, lossless); the DISPLAY (`$$…$$`) path is where full 2-D
    // layout belongs.
    for (int c = 0; c < b.cols; ++c) {
        const Cell& cell = b.at(b.axis, c);
        if (cell.len && std::string_view(cell.bytes.data(), cell.len) == "\u2500")
            return std::string(latex);
    }
    return row_text(b.axis);
}

} // namespace maya::texmath
