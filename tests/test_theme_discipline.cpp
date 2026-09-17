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
        // A guard is CODE. Reading it out of a comment is not a detail:
        // every one of these functions carries a comment explaining the
        // has_channels hazard, so honouring comments disarmed the scanner
        // in exactly the files that needed it most. anim::lerp sat under a
        // 20-line comment headed "Why this checks has_channels()" and was
        // waved through with the guard deleted.
        const auto first = line.find_first_not_of(" \t");
        const bool comment =
            first != std::string::npos
            && (line.compare(first, 2, "//") == 0
                || line.compare(first, 2, "/*") == 0
                || line.compare(first, 1, "*") == 0);

        // What counts as establishing "this colour has channels".
        //
        // has_channels() is the name, but a proven Kind::Rgb is the same
        // fact stated the long way, and the emission paths use it because
        // they must switch on the kind anyway:
        //
        //     case ColorKind::Rgb:            // canvas.cpp, grid_emit.cpp
        //     if (c.kind() == ColorKind::Rgb) // serialize.cpp
        //
        // Inside those, r()/g()/b() ARE channels. Recognising the branch
        // keeps the rule honest — the alternative is an allowlist of file
        // paths, which grants the exemption to the whole file forever
        // rather than to the one branch that earned it.
        const bool establishes =
            line.find("has_channels") != std::string::npos
            || line.find("ColorKind::Rgb") != std::string::npos
            || line.find("Kind::Rgb") != std::string::npos;

        if (!comment && establishes)
            guarded_depth_ = depth_;

        if (comment) return;   // braces in prose are not scope, either

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

    // A guard in a COMMENT is not a guard — the load-bearing one. Every
    // function near this hazard documents it, so honouring comments
    // disarmed the scanner in precisely the files that needed it: lerp sat
    // under "// Why this checks has_channels()" and was waved through with
    // the guard itself deleted.
    {
        const auto g = scan({
            "// Why this checks has_channels() ...",
            "LitColor lerp(LitColor a, LitColor b, double t) {",
            "    return LitColor::rgb(mix(a.r(), b.r()));",
            "}",
        });
        CHECK(!g[0], "a comment mentioning has_channels establishes nothing");
        CHECK(!g[2], "so the body is UNGUARDED and must be flagged");
    }

    // A proven Kind::Rgb branch is the same fact stated the long way.
    {
        const auto g = scan({
            "switch (c.kind()) {",
            "    case ColorKind::Rgb:",
            "        return write(p, c.r(), c.g(), c.b());",
            "}",
        });
        CHECK(g[1], "case ColorKind::Rgb establishes channels");
        CHECK(g[2], "and covers the emission inside it");
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

// Channel arithmetic, across the WHOLE tree.
//
// This is a separate pass from the one above because it has a different
// scope, and getting that wrong is how #45 survived its own fix. The
// colour-literal rule is genuinely widget-shaped: a widget must name a
// role. But reading a palette index as a colour channel is a hazard
// wherever a colour is touched — and the ORIGINAL bug was in
// include/maya/core/animation.hpp, which the widget-only walk never looked
// at. A guard that cannot see the file the bug was in is not a guard.
//
// The rule: resolving a slot gets you a LitColor, which is PAINTABLE but
// not necessarily NUMERIC. Only Kind::Rgb has channels; Named and Indexed
// keep a PALETTE INDEX in the r_ byte with g_/b_ zero, and Default has
// nothing at all. theme::native states every slot as Named or Default on
// purpose, so that the user's own palette reaches the screen — which makes
// every unguarded blend wrong under precisely the theme nobody tests.
//
//     bright_black -> Named(8) -> lerp -> rgb(8,0,0) -> "38;2;8;0;0"
//
// Legitimate reads are EMISSION of a palette index, which spells itself
// index(), and arithmetic guarded by has_channels() — tracked across the
// enclosing block by GuardScope.
TEST_CASE("theme discipline: no unguarded channel arithmetic anywhere") {
    std::println("--- channel arithmetic sweep ---");

    // Every directory that touches a colour, not just the widgets.
    const std::vector<fs::path> roots = {
        fs::path{MAYA_SOURCE_DIR} / "include" / "maya",
        fs::path{MAYA_SOURCE_DIR} / "src",
    };

    // ANY channel read — not just one adjacent to an operator.
    //
    // The first version of this regex required a [-+*/] next to the read.
    // That felt precise and was useless: the original #45 bug passes its
    // channels to a FUNCTION,
    //
    //     return LitColor::rgb(mix(a.r(), b.r()), ...);
    //
    // so no operator is in sight and the scanner walked straight past the
    // very line it was written for. Verified by deleting the lerp guard and
    // re-running the sweep: zero hits on animation.hpp.
    //
    // So flag EVERY read and make the legitimate ones say so. There are
    // exactly two legitimate shapes and both are already explicit:
    //
    //   * EMISSION of a palette index — spells itself index().
    //   * Arithmetic proven safe — carries has_channels() in its block.
    //
    // Anything else reads bytes whose meaning depends on a kind it never
    // checked.
    const std::regex channel_read{R"(\.[rgb]\(\))"};

    // color.hpp is the ONE file exempt, and only because it is the type
    // itself: its static_asserts read channels off a literal hex() (whose
    // kind is Rgb by construction, at compile time) and CanReadChannels is
    // a concept probe that never runs. A blend cannot hide here — there is
    // nothing in this file to blend WITH.
    const std::string self = "color.hpp";

    std::vector<std::string> offenders;
    int scanned = 0;

    for (const auto& root : roots) {
        if (!fs::exists(root)) continue;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension();
            if (ext != ".hpp" && ext != ".cpp") continue;
            if (e.path().filename().string() == self) continue;
            ++scanned;

            std::ifstream in{e.path()};
            std::string line;
            int n = 0;
            GuardScope scope;
            while (std::getline(in, line)) {
                ++n;
                scope.observe(line);
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos
                    && (line.compare(first, 2, "//") == 0
                        || line.compare(first, 1, "*") == 0))
                    continue;
                if (std::regex_search(line, channel_read)
                    && !scope.guarded())
                    offenders.push_back(
                        fs::relative(e.path(), MAYA_SOURCE_DIR).string() + ":"
                        + std::to_string(n) + "  " + line);
            }
        }
    }

    // Guard the guard: the widget-only version of this check scanned a
    // directory that did not contain the bug and passed forever.
    assert(scanned > 100);

    if (!offenders.empty()) {
        std::println("{} unguarded channel read(s):", offenders.size());
        for (std::size_t i = 0; i < offenders.size() && i < 25; ++i)
            std::println("   {}", offenders[i]);
        std::println("\nOnly Kind::Rgb has channels. Guard with has_channels(),");
        std::println("or spell a palette-index read index(). See #45.");
    }
    assert(offenders.empty());

    std::println("PASS ({} files scanned)\n", scanned);
}
