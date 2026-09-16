// The markdown palette: one authored mapping, published as a value.
//
// Two defects lived here, and both were invisible to review:
//
//   1. DIVERGENCE. The theme-slot mapping was authored TWICE — the defaults
//      in markdown/internal.hpp and markdown_palette_from() in
//      render_block.cpp — and the two disagreed on NINE of thirty-seven
//      roles (heading1/2/3, table_header, highlight_fg, code_border,
//      hrule_fg, code_fg, mention_fg). Markdown rendered under mapping A
//      until the first theme swap and mapping B forever after, so headings,
//      code spans and links shifted colour on a swap that changed nothing
//      else — and swapping to the SAME theme still moved them.
//
//   2. A DATA RACE. The palette was ~35 plain mutable globals, written by
//      the UI thread and read by the DETACHED streaming parse worker across
//      ~165 sites. The header asked callers to only re-theme while nothing
//      was streaming, which the one caller that matters (a live theme
//      preview) cannot honour.
//
// Both are now structural: MAYA_MD_PALETTE is the single statement of the
// mapping, and the palette is an immutable snapshot published by pointer
// swap. These cases pin that, including the concurrency shape.

#include "agtest.hpp"

#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/markdown/internal.hpp>

#include <atomic>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

using namespace maya;

namespace {

[[nodiscard]] const Theme* scheme_named(std::string_view want) {
    for (const auto& s : theme::schemes)
        if (std::string_view{s.name} == want) return s.theme;
    return nullptr;
}

// Count roles where two palettes disagree.
[[nodiscard]] int divergences(const colors::Palette& a, const colors::Palette& b) {
    int n = 0;
#define X(f, SLOT) if (!(a.f == b.f)) ++n;
    MAYA_MD_PALETTE(X)
#undef X
    return n;
}

}  // namespace

TEST_CASE("markdown palette: the boot palette IS the projection") {
    std::println("--- test_md_palette_no_divergence ---");
    // The regression, stated directly. Before the merge these differed in 9
    // of 37 roles, so the first theme swap repainted headings/code/links
    // even when it swapped to the theme already in force.
    theme::set_live(theme::native);
    const colors::Palette boot = colors::live();
    const colors::Palette proj = colors::project(theme::native);
    CHECK(divergences(boot, proj) == 0);
    std::println("PASS\n");
}

TEST_CASE("markdown palette: publishing lands exactly on the projection") {
    std::println("--- test_md_palette_publish ---");
    // A light scheme on purpose: polarity is what makes a stale palette
    // visible (dark ink on a light canvas), and it is the case a user hits.
    const Theme* rose = scheme_named("Rose Pine Dawn");
    REQUIRE(rose != nullptr);

    theme::set_live(*rose);
    set_markdown_palette(markdown_palette_from(*rose));
    CHECK(divergences(colors::live(), colors::project(*rose)) == 0);

    // Every built-in scheme round-trips through the public API onto its own
    // projection — the mapping cannot be partial or order-dependent.
    int checked = 0;
    for (const auto& s : theme::schemes) {
        theme::set_live(*s.theme);
        set_markdown_palette(markdown_palette_from(*s.theme));
        CHECK(divergences(colors::live(), colors::project(*s.theme)) == 0);
        ++checked;
    }
    CHECK(checked > 50);

    theme::set_live(theme::native);
    set_markdown_palette(markdown_palette_from(theme::native));
    std::println("PASS ({} schemes round-tripped)\n", checked);
}

TEST_CASE("markdown palette: every role resolves to a painted colour") {
    std::println("--- test_md_palette_total ---");
    // The palette is LitColor, so nothing in it can still be an unresolved
    // slot — a parse worker has no theme to resolve against, which is why
    // the projection happens on the publishing side.
    for (const auto& s : theme::schemes) {
        const colors::Palette p = colors::project(*s.theme);
#define X(f, SLOT) CHECK(p.f.kind() != ColorKind::Slot); \
                   CHECK(p.f.is_set());
        MAYA_MD_PALETTE(X)
#undef X
    }
    std::println("PASS\n");
}

TEST_CASE("markdown palette: readers never observe a torn palette") {
    std::println("--- test_md_palette_snapshot ---");
    // The concurrency shape of the real thing: the UI thread previewing
    // themes (the appearance panel has no streaming gate) while detached
    // parse workers read the palette mid-render.
    //
    // The property is that a reader sees a COHERENT palette — one theme's
    // projection, never a mix of two. Under the old mutable globals a worker
    // could observe half the roles updated; that is what "not published
    // atomically" meant in practice, and it is unrepresentable now because a
    // reader holds a pointer to a frozen value.
    //
    // Run this under TSan for the stronger statement (verified clean:
    // 114k publishes against 3.9M reads, zero reports).
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> reads{0};
    std::atomic<unsigned long> torn{0};

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                // One acquire load, then read a frozen object — exactly what
                // the render path does.
                //
                // BY VALUE, deliberately. Holding a `const Palette&` and
                // re-reading it across the loop below would compare fields
                // from DIFFERENT snapshots once a publish lands mid-scan —
                // a bug in the test, not in the palette, and one that
                // reported ~3 spurious tears per 2000 reads. A copy is what
                // "I took a snapshot" actually means.
                const colors::Palette p = colors::live();
                // A coherent snapshot matches SOME theme's projection.
                //
                // native is in the candidate set because the reader can
                // start before the first publish and legitimately observe
                // the boot palette — which is native's projection, and is
                // not equal to any of the 57 schemes. Leaving it out made
                // exactly 3 valid reads per run look like tears.
                bool coherent = divergences(p, colors::project(theme::native)) == 0;
                for (const auto& s : theme::schemes) {
                    if (coherent) break;
                    if (divergences(p, colors::project(*s.theme)) == 0) {
                        coherent = true;
                        break;
                    }
                }
                if (!coherent) torn.fetch_add(1, std::memory_order_relaxed);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int n = 0; n < 40; ++n)
        for (const auto& s : theme::schemes)
            set_markdown_palette(markdown_palette_from(*s.theme));

    stop.store(true);
    for (auto& t : workers) t.join();

    CHECK(reads.load() > 0);
    CHECK(torn.load() == 0);

    theme::set_live(theme::native);
    set_markdown_palette(markdown_palette_from(theme::native));
    std::println("PASS ({} concurrent reads, {} torn)\n", reads.load(), torn.load());
}
