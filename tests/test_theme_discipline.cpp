// theme_discipline_test — the theme system's own guard rail.
//
// The rule: a widget names a ROLE, the theme decides what the role looks
// like. Every regression in this area has been the same shape — a colour
// literal in a Config default, evaluated at static-init with no theme in
// scope, silently pinning that widget to the palette it was compiled with.
// You cannot see it in review and you cannot see it until someone picks a
// theme and half the screen ignores them.
//
// So it is checked mechanically, against the source, on every build.
#include <maya/maya.hpp>
#undef NDEBUG
#include "agtest.hpp"

#include <filesystem>
#include <fstream>
#include <print>
#include <regex>
#include <string>
#include <vector>

using namespace maya;
namespace fs = std::filesystem;

namespace {

// Literals that are legitimately NOT theme slots.
//
// A theme assigns colour to ROLES. These name something else, so a slot
// would be the wrong answer, not merely a stricter one:
//
//   * markdown/highlight.hpp — the named syntax decks (Monokai, ...). A
//     deck IS a palette; resolving it through the theme would make every
//     deck identical.
//   * file_tree / editor_tab_bar — language BRAND colours (Rust orange, Go
//     cyan). They identify a language, not a UI role, and are the same hue
//     in every editor precisely because they are not themeable.
const std::vector<std::string> kAllowed = {
    "markdown/highlight.hpp",
    "file_tree.hpp",
    "editor_tab_bar.hpp",
};

[[nodiscard]] bool allowed(const std::string& path) {
    for (const auto& a : kAllowed)
        if (path.size() >= a.size()
            && path.compare(path.size() - a.size(), a.size(), a) == 0)
            return true;
    return false;
}

// ── Is this channel read guarded? ───────────────────────────────────────
//
// has_channels() establishes a fact about a colour that holds for the REST
// OF THE ENCLOSING BLOCK, not for one line. Checking a single line forces
// the marker onto the arithmetic itself, which is unreadable and fails open
// on the natural spelling:
//
//     if (!c.has_channels()) return c;
//     return LitColor::rgb(c.r() / 2, ...);   // correct, but flagged
//
// So track brace depth: a guard seen at depth d covers every line until the
// depth drops back below d, which is the scope where the fact still holds.
//
// A heuristic, not a parser — braces inside strings or comments skew the
// depth. Deliberately biased toward FALSE POSITIVES (a skew expires the
// guard early and flags a safe line): a nuisance failure costs a minute,
// a missed one ships #45 again.
class GuardScope {
public:
    void observe(const std::string& line) {
        if (line.find("has_channels") != std::string::npos)
            guarded_depth_ = depth_;
        for (char c : line) {
            if (c == '{') ++depth_;
            else if (c == '}') {
                --depth_;
                if (guarded_depth_ >= 0 && depth_ < guarded_depth_)
                    guarded_depth_ = -1;
            }
        }
    }
    [[nodiscard]] bool guarded() const noexcept { return guarded_depth_ >= 0; }
private:
    int depth_ = 0;
    int guarded_depth_ = -1;
};

}  // namespace

// The scanner's own unit test.
//
// A source-grep guard is itself code, and a guard that silently stops
// guarding is worse than none — it reads as green forever. The line-scoped
// version of this check failed exactly that way: it accepted only a marker
// on the arithmetic line, so every correctly-written guard was a false
// positive and the obvious fix ("add // has_channels to the line") taught
// the wrong model of the predicate. Pin the scope rules directly.
TEST_CASE("theme discipline: the guard tracker is block-scoped") {
    auto scan = [](std::initializer_list<const char*> lines) {
        GuardScope s;
        std::vector<bool> guarded;
        for (const char* l : lines) {
            const std::string line{l};
            s.observe(line);
            guarded.push_back(s.guarded());
        }
        return guarded;
    };

    // A guard covers the rest of its block — the case that forced the ugly
    // inline marker in ui_theme.hpp.
    {
        const auto g = scan({
            "bool f(LitColor c) {",              // 0: no guard yet
            "    if (!c.has_channels()) return false;", // 1: guard established
            "    return c.r() + c.g() > 10;",    // 2: STILL guarded
            "}",                                 // 3: block closed
        });
        CHECK(!g[0], "no guard before has_channels is seen");
        CHECK(g[1],  "the guard line itself counts");
        CHECK(g[2],  "the guard must survive to the next line — this is the "
                     "whole point of tracking scope rather than lines");
        CHECK(!g[3], "the guard expires when its block closes");
    }

    // A guard inside a NESTED block does not leak out of it.
    {
        const auto g = scan({
            "void f() {",                        // 0
            "    if (x) {",                      // 1
            "        if (c.has_channels()) {}",  // 2: guarded at depth 2
            "    }",                             // 3: depth 1 — expired
            "    return c.r() * 2;",             // 4: unguarded again
            "}",                                 // 5
        });
        CHECK(g[2],  "guarded inside the nested block");
        CHECK(!g[3], "leaving the block drops the guard");
        CHECK(!g[4], "a sibling statement is NOT covered by it");
    }

    // Two functions in a row: the first one's guard must not cover the
    // second. This is the fail-open case that would hide a real bug.
    {
        const auto g = scan({
            "int a(LitColor c) {",
            "    if (!c.has_channels()) return 0;",
            "    return c.r() / 2;",
            "}",
            "int b(LitColor c) {",
            "    return c.r() / 2;",             // 5: MUST be flagged
            "}",
        });
        CHECK(g[2],  "the first function is guarded");
        CHECK(!g[5], "the next function must NOT inherit the guard");
    }
}

TEST_CASE("theme discipline: widgets name roles, not colours") {
    std::println("--- test_theme_discipline ---");

    const fs::path root = fs::path{MAYA_SOURCE_DIR} / "include" / "maya" / "widget";
    REQUIRE(fs::exists(root));

    // A bare named/hex/indexed colour, anywhere it is USED rather than
    // described. Color::slot(), Color::default_color() and computed
    // Color::rgb(expr) are all fine — the first is the point, the second is
    // "the terminal's own", and the third is arithmetic (gradients, hashes).
    const std::regex lit{
        R"(Color::(black|red|green|yellow|blue|magenta|cyan|white|bright_\w+)\(\))"
        R"(|Color::hex\(0x)"
        R"(|Color::indexed\()"};

    std::vector<std::string> offenders;
    int scanned = 0;

    // A slot in a BACKGROUND position must be a surface slot.
    //
    // Ink slots (text, accents) move opposite to the canvas when a theme
    // flips polarity, so using one as a fill inverts: a tint that reads as a
    // subtle wash on a dark theme becomes a near-black slab on a light one.
    // The bulk conversion introduced five of these and none were visible in
    // review — both sides look deliberate in the diff.
    const std::regex ink_as_bg{
        R"((with_bg|_bg|\bbg\b|bgc)\s*[=(][^;]*ThemeSlot::)"
        R"((Text|Secondary|Muted|Primary|Accent|Info|Link|Success|Warning|Error|Placeholder|Cursor)\b)"};

    // Reading a Color's CHANNELS without resolving it first.
    //
    // A slot carries its enum in the red byte, so to_rgb() on an unresolved
    // slot reads the slot NUMBER as an ANSI index and returns a plausible,
    // wrong colour — never a diagnostic. Anything built on those bytes
    // (equality, luminance, interpolation, hashing) quietly answers nonsense.
    //
    // This is the second-order hazard of slots and it bit four times: the
    // sigil's colour comparison (every half-block took the mixed branch and
    // painted a dark slab on light themes), its cache key (all slots hashed
    // alike), the gradient ramp, and the grid backend's wire encoder. All
    // four were invisible until a light theme made them obvious.
    //
    // Matches `.to_rgb()` on a line that does not also resolve — the one
    // call that silently fabricates channels.
    const std::regex unresolved_to_rgb{R"(\.to_rgb\(\))"};

    // Doing ARITHMETIC on a colour's channel bytes.
    //
    // The hazard above, one level deeper. Resolving a slot gets you a
    // LitColor, which is PAINTABLE but not necessarily NUMERIC: only
    // Kind::Rgb has channels. Named and Indexed keep a PALETTE INDEX in the
    // r_ byte with g_/b_ zero, and Default has nothing at all — and
    // theme::native states every one of its slots as Named or Default, on
    // purpose, so the user's own palette reaches the screen.
    //
    // So `r() / 4`, `(r() + x) / 2`, `r() - other.r()` and friends are
    // reading a palette index as a colour channel. That is agentty #45:
    // bright_black (Named 8) blended to rgb(8,0,0), a near-black truecolor
    // triple painted over every line of every reasoning block, invisible on
    // a dark terminal and undetectable to anyone running a scheme.
    //
    // Legitimate uses of these bytes are EMISSION (writing the palette
    // index into an SGR sequence), which spells itself index(), and
    // arithmetic guarded by has_channels() — tracked across the enclosing
    // block by GuardScope above. This matches a channel read adjacent to an
    // operator, so emission and comparison stay clean.
    const std::regex channel_arithmetic{
        R"(\.[rgb]\(\)\s*[-+*/]|[-+*/]\s*\w*\.[rgb]\(\))"};

    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".hpp") continue;
        const std::string path = e.path().string();
        if (allowed(path)) continue;
        ++scanned;

        std::ifstream in{e.path()};
        std::string line;
        int n = 0;
        GuardScope scope;
        while (std::getline(in, line)) {
            ++n;
            scope.observe(line);
            // Skip comments — examples in docs are not code.
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string::npos
                && (line.compare(first, 2, "//") == 0
                    || line.compare(first, 1, "*") == 0)) continue;
            if (std::regex_search(line, lit))
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n) + "  " + line);
            if (std::regex_search(line, ink_as_bg))
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n)
                                    + "  (ink slot as background)  " + line);
            if (std::regex_search(line, unresolved_to_rgb)
                && line.find("resolve") == std::string::npos)
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n)
                                    + "  (to_rgb without resolve)  " + line);
            if (std::regex_search(line, channel_arithmetic) && !scope.guarded())
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n)
                                    + "  (channel arithmetic, unguarded)  " + line);
        }
    }

    // Guard the guard: if the walk found nothing to scan, the test is
    // passing for the wrong reason.
    assert(scanned > 20);

    if (!offenders.empty()) {
        std::println("{} widget colour literal(s) that should be theme slots:",
                     offenders.size());
        for (std::size_t i = 0; i < offenders.size() && i < 25; ++i)
            std::println("   {}", offenders[i]);
        std::println("\nUse Color::slot(ThemeSlot::X) so the colour follows the");
        std::println("user's theme. If the literal is genuinely not a UI role");
        std::println("(a language brand colour, a named syntax deck), add the");
        std::println("file to kAllowed in this test with the reason why.");
    }
    assert(offenders.empty());

    std::println("PASS ({} widget headers scanned)\n", scanned);
}
