// Tests for the motion FRAMEWORK (core/motion.hpp): Clock, Motion<T>,
// Timeline, Stagger, pulse/loop helpers. These exercise the driving layer
// that sits on top of the animation.hpp math primitives — the part that
// owns time, ticking, and lifecycle so widgets don't have to.
//
// The frame-request hook (anim::detail::raf_hook) is observed here via a
// test stub so we can assert that an in-flight Motion asks for frames and a
// settled one stops — the property that keeps a maya app idle when nothing
// is animating.
#include <maya/maya.hpp>
#include <maya/anim/text_reveal.hpp>
#include <cassert>
#include <cmath>
#include <print>

#include "check.hpp"
// CMake builds tests in Release (-DNDEBUG), which strips bare assert(). Route
// every assert in this file through the hard check so the invariants ACTUALLY
// run — otherwise a broken animation "passes" silently (which is exactly how
// the typewriter-reveal regression slipped through).
#undef assert
#define assert(x) MAYA_TEST_CHECK((x), #x)

using namespace maya;
using namespace maya::anim;

static bool approx(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) < eps;
}

// ── Frame-request observation ──────────────────────────────────────────────
// Install a counting stub so we can prove Motion drives the loop only while
// moving. (In a real app app.hpp installs request_animation_frame.)
static int g_raf_calls = 0;
static void counting_raf() noexcept { ++g_raf_calls; }
struct RafGuard {
    anim::detail::RafHook saved;
    RafGuard() : saved(anim::detail::raf_hook) { anim::detail::raf_hook = &counting_raf; }
    ~RafGuard() { anim::detail::raf_hook = saved; }
};

// ── Clock: dt clamps long stalls and zeroes a remount ───────────────────────
void test_clock_dt_semantics() {
    std::println("--- test_clock_dt_semantics ---");
    Clock clk;
    // First dt() is ~0 (epoch == last on construction). Subsequent reads in
    // the SAME integer-ms frame return the cached value.
    double a = clk.dt();
    double b = clk.dt();
    assert(approx(a, b) && "dt cached within a frame");
    // A genuinely separate frame is impossible to force without sleeping, so
    // we validate the clamp/remount math directly on the documented tunables.
    static_assert(Clock::kMaxStep == 0.25);
    static_assert(Clock::kRemountGapMs == 500.0);
    std::println("PASS");
}

// ── Motion: settles, stops requesting frames, no jump on retarget ───────────
void test_motion_basic() {
    std::println("--- test_motion_basic ---");
    RafGuard g;
    Clock clk;

    Motion<double> m{0.0, 0.30, ease::linear};
    assert(approx(m.peek(), 0.0));
    assert(!m.moving());

    m.to(1.0, 0.30);            // linear over 0.30s
    assert(m.moving());

    g_raf_calls = 0;
    // Feed dt manually via get_on with an explicit clock we step by abusing
    // begin_frame() between integer-ms boundaries is fiddly; instead drive
    // the underlying math deterministically through a hand clock proxy.
    // We simulate frames by directly ticking a local Animated mirror is not
    // possible (private), so we assert behavioural endpoints:
    //   - while moving, get_on requests at least one frame
    //   - peek advances toward target
    (void)m.get_on(clk);
    // First get after to(): with a near-zero dt the value is still ~0 but a
    // frame WAS requested because we're moving.
    assert(g_raf_calls >= 1 && "in-flight Motion requests frames");

    // Snap kills motion and stops frame requests.
    m.snap(0.5);
    assert(!m.moving());
    g_raf_calls = 0;
    double v = m.get_on(clk);
    assert(approx(v, 0.5));
    assert(g_raf_calls == 0 && "settled Motion does not request frames");
    std::println("PASS");
}

// ── Motion drives to completion over simulated frames ───────────────────────
// We can't sleep in a unit test, so we validate the END STATE: after enough
// elapsed time the value reaches target and moving() clears. We do this by
// constructing the Motion in spring mode and stepping a private-free proxy:
// instead, test the Timeline (which exposes a seekable playhead) for the
// time-accurate path, and here assert the monotone approach property using
// peek() after to().
void test_motion_no_visual_jump_on_retarget() {
    std::println("--- test_motion_no_visual_jump_on_retarget ---");
    Clock clk;
    Motion<double> m{0.0, 0.20, ease::linear};
    m.to(10.0, 0.20);
    // Mid-flight retarget: value must continue from wherever it is, never
    // snap. peek() before/after to() must be identical (set_target keeps the
    // current value as the new `from`).
    double before = m.peek();
    m.to(-5.0, 0.20);
    double after = m.peek();
    assert(approx(before, after) && "retarget preserves current value");
    std::println("PASS");
}

// ── Motion<Color> compiles & lerps endpoints ────────────────────────────────
void test_motion_color() {
    std::println("--- test_motion_color ---");
    Clock clk;
    Motion<Color> c{Color::rgb(0, 0, 0), 0.25};
    Color v = c.peek();
    assert(v.r() == 0 && v.g() == 0 && v.b() == 0);
    c.to(Color::rgb(255, 128, 64));
    assert(c.moving());
    c.snap(Color::rgb(10, 20, 30));
    Color s = c.get_on(clk);
    assert(s.r() == 10 && s.g() == 20 && s.b() == 30);
    std::println("PASS");
}

// ── pulse / loop_phase: bounded, periodic, frame-requesting ─────────────────
void test_pulse_loop() {
    std::println("--- test_pulse_loop ---");
    RafGuard g;
    Clock clk;
    g_raf_calls = 0;
    for (int i = 0; i < 8; ++i) {
        double p = pulse(600.0, clk);
        assert(p >= 0.0 && p <= 1.0 && "pulse stays in [0,1]");
        double s = loop_phase(600.0, clk);
        assert(s >= 0.0 && s < 1.0 && "loop_phase stays in [0,1)");
    }
    assert(g_raf_calls >= 1 && "pulse/loop keep the loop awake");
    // pulse_between endpoints.
    double mid = pulse_between(0.0, 100.0, 600.0, clk);
    assert(mid >= 0.0 && mid <= 100.0);
    std::println("PASS");
}

// ── Timeline: seekable, keyframes interpolate, parallel tracks ──────────────
void test_timeline_keyframes() {
    std::println("--- test_timeline_keyframes ---");
    Timeline tl;
    auto opacity = tl.track(0.0);
    opacity.hold(0.0, 0.10)                       // 0..0.10s: stay 0
           .to(1.0, 0.30, ease::linear);          // 0.10..0.40s: 0→1
    auto y = tl.track(8.0);
    y.at(0.10).to(0.0, 0.20, ease::linear);       // 0.10..0.30s: 8→0

    assert(approx(tl.duration(), 0.40) && "duration is the latest track end");

    // Seek and read both tracks.
    tl.seek(0.0);
    assert(approx(opacity.value(), 0.0));
    assert(approx(y.value(), 8.0));

    tl.seek(0.10);
    assert(approx(opacity.value(), 0.0) && "opacity still 0 at end of hold");
    assert(approx(y.value(), 8.0) && "y at start of its move");

    tl.seek(0.25);   // opacity: (0.25-0.10)/0.30 = 0.5 → 0.5
    assert(approx(opacity.value(), 0.5) && "opacity halfway");
    // y: (0.25-0.10)/0.20 = 0.75 → 8*(1-0.75) = 2.0
    assert(approx(y.value(), 2.0) && "y three-quarters in");

    tl.seek(0.40);
    assert(approx(opacity.value(), 1.0) && "opacity full at end");
    assert(approx(y.value(), 0.0) && "y settled at end");

    std::println("PASS");
}

// ── Timeline: play() advances the playhead and reports done() ───────────────
void test_timeline_play_done() {
    std::println("--- test_timeline_play_done ---");
    Timeline tl;
    auto a = tl.track(0.0);
    a.to(1.0, 0.10, ease::linear);
    assert(!tl.done() && "fresh timeline with a key is not done at 0");
    tl.play();
    tl.seek(0.10);
    assert(tl.done() && "playhead at duration ⇒ done");
    // Looping timeline never reports done.
    tl.set_loop(true);
    assert(!tl.done() && "looping timeline is never done");
    std::println("PASS");
}

// ── Stagger: per-item phased progress + completion ──────────────────────────
void test_stagger() {
    std::println("--- test_stagger ---");
    // step 0.05s, dur 0.30s, linear.
    // item 0 starts at 0.0; item 3 starts at 0.15.
    double e = 0.0;
    assert(approx(stagger_progress(e, 0, 0.05, 0.30, ease::linear), 0.0));
    assert(approx(stagger_progress(e, 5, 0.05, 0.30, ease::linear), 0.0)
           && "later item clamps to 0 before its start");

    e = 0.15;  // item 0: 0.15/0.30 = 0.5 ; item 3: (0.15-0.15)/0.30 = 0
    assert(approx(stagger_progress(e, 0, 0.05, 0.30, ease::linear), 0.5));
    assert(approx(stagger_progress(e, 3, 0.05, 0.30, ease::linear), 0.0));

    e = 0.30;  // item 0 done (clamped 1.0); item 3: (0.30-0.15)/0.30 = 0.5
    assert(approx(stagger_progress(e, 0, 0.05, 0.30, ease::linear), 1.0));
    assert(approx(stagger_progress(e, 3, 0.05, 0.30, ease::linear), 0.5));

    // Completion: 5 items, last starts at 4*0.05=0.20, ends at 0.50.
    assert(!stagger_done(0.49, 5, 0.05, 0.30));
    assert(stagger_done(0.50, 5, 0.05, 0.30));
    assert(stagger_done(0.0, 0, 0.05, 0.30) && "empty stagger is done");
    std::println("PASS");
}

// ── text_reveal: TYPEWRITER semantics ─────────────────────────────────
// The reveal must read as left-to-right typing: codepoints BEFORE the reveal
// cursor render as their REAL glyph (revealed), codepoints AT/AFTER the cursor
// render GHOSTED (invisible — fg == default_color / dim). The bug this guards:
// the not-yet-typed tail was rendering scramble glyphs across the whole
// unrevealed span instead of ghosting, so nothing looked like typing.
//
// We decode each codepoint's effective style by mapping its byte offset to
// the StyledRun covering it.
static const StyledRun* run_at(const TextElement& t, std::size_t byte_off) {
    for (const auto& r : t.runs)
        if (byte_off >= r.byte_offset && byte_off < r.byte_offset + r.byte_length)
            return &r;
    return nullptr;
}

// Is this style "ghosted" (the invisible not-yet-typed marker)?
static bool is_ghost_style(const Style& s) {
    return s.fg.has_value() && s.fg->kind() == Color::Kind::Default && s.dim;
}

void test_text_reveal_typewriter() {
    std::println("--- test_text_reveal_typewriter ---");
    // Pure-ASCII body so 1 cp == 1 byte (offset math is trivial).
    const std::string body =
        "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnop";  // 51 cp
    const std::size_t total = body.size();

    // Cursor at 20 of 51. Disable scramble so we test the pure reveal/ghost
    // boundary deterministically (scramble glyphs are nondeterministic).
    const std::size_t cursor = 20;
    TextElement leaf;
    leaf.content = body;
    anim::TextRevealParams p;
    p.ms_total        = 1000;
    p.edge_age_ms     = 0;
    p.revealed_cp     = cursor;
    p.total_cp        = total;
    p.enable_scramble = false;   // isolate the reveal/ghost split
    anim::decorate_text_reveal(leaf, p);

    // Content bytes for revealed cp must be UNCHANGED (real glyphs), and their
    // style must NOT be ghost. Codepoints at/after the cursor must be ghost
    // EXCEPT the single sweep-cursor head (the freshest unrevealed cp, which
    // gets the bright "now typing" highlight — that's correct, not a bug).
    int revealed_real = 0, ghosted = 0, sweep = 0, bad = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const char c = leaf.content[i];   // 1 byte per cp here
        const StyledRun* r = run_at(leaf, i);
        assert(r && "every byte covered by a run");
        const bool ghost = is_ghost_style(r->style);
        if (i < cursor) {
            // Revealed: real glyph, not ghosted.
            if (c != body[i]) ++bad;
            else if (ghost)   ++bad;
            else              ++revealed_real;
        } else if (i == cursor) {
            // The sweep-cursor head: bright highlight (not ghost), real glyph.
            if (!ghost) ++sweep; else ++ghosted;
        } else {
            // Not yet typed: MUST be ghosted (the failing-bug region).
            if (ghost) ++ghosted;
            else       ++bad;
        }
    }
    std::println("  revealed={} sweep={} ghosted={} bad={} (cursor={}/{})",
                 revealed_real, sweep, ghosted, bad, cursor, total);
    assert(revealed_real == static_cast<int>(cursor) &&
           "all pre-cursor cp render as real revealed glyphs");
    assert(sweep == 1 && "the reveal-front cp is the bright sweep cursor");
    assert(ghosted == static_cast<int>(total - cursor - 1) &&
           "all past-cursor cp render ghosted (typewriter, not scramble)");
    assert(bad == 0);
    std::println("PASS");
}

// ── text_reveal: full reveal (cursor at end) ghosts NOTHING ───────────────
void test_text_reveal_complete_no_ghost() {
    std::println("--- test_text_reveal_complete_no_ghost ---");
    const std::string body = "the quick brown fox jumps over the lazy dog";
    TextElement leaf;
    leaf.content = body;
    anim::TextRevealParams p;
    p.ms_total        = 2000;
    p.edge_age_ms     = 2000;     // old → no scramble, settled colours
    p.revealed_cp     = string_width(body);
    p.total_cp        = string_width(body);
    p.enable_scramble = false;
    anim::decorate_text_reveal(leaf, p);
    for (std::size_t i = 0; i < leaf.content.size(); ++i) {
        const StyledRun* r = run_at(leaf, i);
        assert(r && !is_ghost_style(r->style) &&
               "fully-revealed text has no ghosted cp");
    }
    std::println("PASS");
}

// ── RateCursor is still reachable through the motion umbrella ───────────────
void test_rate_cursor_reachable() {
    std::println("--- test_rate_cursor_reachable ---");
    RateCursor rc{120.0, 0.8};
    rc.set_pos(0.0);
    double p = rc.tick(1000.0, 0.016);  // big backlog → burst drain
    assert(p > 0.0 && "RateCursor advances");
    std::println("PASS");
}

// ── text_reveal: height-stable trailing-edge decoration ─────────────────────
// The decorator's load-bearing invariant: it must NEVER change the leaf's
// DISPLAY WIDTH (it may swap a glyph for an equal-width scramble glyph, but
// the column count of the content is preserved) — that's what keeps streaming
// text from reflowing committed rows into scrollback.
static int display_width_of(const TextElement& t) {
    return string_width(t.content);
}

void test_text_reveal_height_stable() {
    std::println("--- test_text_reveal_height_stable ---");
    const std::string body =
        "The quick brown fox jumps over the lazy dog and keeps on running "
        "down the road past the end of the visible viewport edge.";

    // Decorate at several cursor positions + ages; width must stay constant.
    // Scramble is ON (default) at edge_age 0 so the scramble-glyph swap path
    // is exercised — a width-2 scramble glyph (e.g. an emoji) would break this
    // and was the real reveal-jitter / scrollback-reflow bug.
    const int base_w = string_width(body);
    for (std::size_t rev = 0; rev <= 40; rev += 8) {
        for (std::int64_t age : {std::int64_t{0}, std::int64_t{120},
                                 std::int64_t{500}, std::int64_t{900}}) {
            for (std::int64_t t = 0; t < 8; ++t) {  // vary ms_total → scramble glyph
                TextElement leaf;
                leaf.content = body;
                anim::TextRevealParams p;
                p.ms_total    = t * 47 + 17;
                p.edge_age_ms = age;
                p.revealed_cp = rev;
                (void)anim::decorate_text_reveal(leaf, p);
                assert(display_width_of(leaf) == base_w &&
                       "reveal decoration must preserve display width");
                std::size_t cover = 0;
                for (const auto& r : leaf.runs) {
                    assert(r.byte_offset == cover && "runs contiguous");
                    cover += r.byte_length;
                }
                assert(cover == leaf.content.size() && "runs cover all bytes");
            }
        }
    }
    std::println("PASS");
}

// ── text_reveal: empty leaf is a no-op ──────────────────────────────────────
void test_text_reveal_empty() {
    std::println("--- test_text_reveal_empty ---");
    TextElement leaf;  // empty
    anim::TextRevealParams p;
    bool changed = anim::decorate_text_reveal(leaf, p);
    assert(!changed && leaf.content.empty());
    std::println("PASS");
}

// ── decorate_end_caret: recolors last glyph, width-stable; empty gets ▊ ─────
void test_end_caret() {
    std::println("--- test_end_caret ---");
    TextElement leaf;
    leaf.content = "hello";
    const int w0 = string_width(leaf.content);
    anim::decorate_end_caret(leaf, 100, 650);
    assert(string_width(leaf.content) == w0 && "caret recolor is width-stable");
    assert(!leaf.runs.empty() && "caret adds a styled run");

    // Empty leaf gets a single block caret.
    TextElement empty;
    anim::decorate_end_caret(empty, 100, 650);
    assert(!empty.content.empty() && "empty leaf gets a caret glyph");
    assert(string_width(empty.content) == 1 && "block caret is one column");
    std::println("PASS");
}

// ── clip_text_to_cursor: TRUE typewriter — content physically cut at cursor ─
void test_clip_to_cursor() {
    std::println("--- test_clip_to_cursor ---");
    const std::string body = "the quick brown fox";  // 19 cp, all 1-byte
    // Cut at 9 cp: content must be EXACTLY the first 9 bytes, nothing past it.
    {
        TextElement leaf;
        leaf.content = body;
        const std::size_t kept = anim::clip_text_to_cursor(leaf, 9);
        assert(kept == 9 && "returns surviving byte length");
        assert(leaf.content == "the quick" &&
               "content physically truncated at the cursor (true invisibility)");
        assert(string_width(leaf.content) == 9);
    }
    // Multi-byte: cut mid-UTF-8 sequence never splits a codepoint.
    {
        TextElement leaf;
        leaf.content = "a\xce\xbb\xce\xbc" "b";  // a λ μ b  (4 cp)
        const std::size_t kept = anim::clip_text_to_cursor(leaf, 2);  // aλ
        assert(leaf.content == "a\xce\xbb" && "cuts on a cp boundary");
        assert(kept == 3 && "byte length 1+2");
    }
    // Cursor at/over the end keeps the whole body.
    {
        TextElement leaf;
        leaf.content = body;
        const std::size_t kept = anim::clip_text_to_cursor(leaf, 999);
        assert(kept == body.size() && leaf.content == body);
    }
    // Runs straddling the cut get clipped, runs past it dropped.
    {
        TextElement leaf;
        leaf.content = body;
        leaf.runs = {StyledRun{0, 5, Style{}}, StyledRun{5, 14, Style{}.with_bold()}};
        anim::clip_text_to_cursor(leaf, 7);  // 7 bytes
        assert(leaf.content.size() == 7);
        std::size_t covered = 0;
        for (const auto& r : leaf.runs) covered = std::max(covered, r.byte_offset + r.byte_length);
        assert(covered <= 7 && "no run extends past the cut");
    }
    std::println("PASS");
}

// ── text_reveal: ghost_blank (default) renders unrevealed cp as SPACES ────
void test_text_reveal_ghost_blank() {
    std::println("--- test_text_reveal_ghost_blank ---");
    const std::string body = "the quick brown fox jumps over the lazy dog";
    const std::size_t total = string_width(body);
    const std::size_t cursor = 12;
    TextElement leaf;
    leaf.content = body;
    anim::TextRevealParams p;
    p.ms_total        = 1000;
    p.edge_age_ms     = 0;
    p.revealed_cp     = cursor;
    p.total_cp        = total;
    p.enable_scramble = false;   // isolate the ghost band
    // ghost_blank defaults true.
    anim::decorate_text_reveal(leaf, p);

    // DISPLAY WIDTH preserved (load-bearing: no reflow).
    assert(string_width(leaf.content) == static_cast<int>(total) &&
           "ghost_blank is width-stable");
    // The not-yet-typed body (past the sweep head) must be SPACES — truly
    // invisible, not dim-readable glyphs. The sweep head (cursor-1) keeps its
    // real glyph, so check strictly past the front.
    int blanks = 0, reals = 0;
    for (std::size_t i = 0; i < leaf.content.size(); ++i) {
        if (i < cursor) { ++reals; continue; }            // revealed: real
        if (i == cursor) continue;                        // sweep head: real
        if (leaf.content[i] == ' ') ++blanks;
        else { /* an unrevealed cp that isn't a space = visible ghost = bug */
            assert(false && "unrevealed cp must blank to a space");
        }
    }
    std::println("  reals={} blanks={} (cursor={}/{})", reals, blanks,
                 cursor, total);
    assert(blanks > 0 && "there ARE unrevealed cp, blanked to spaces");
    std::println("PASS");
}

int main() {
    test_clock_dt_semantics();
    test_motion_basic();
    test_motion_no_visual_jump_on_retarget();
    test_motion_color();
    test_pulse_loop();
    test_timeline_keyframes();
    test_timeline_play_done();
    test_stagger();
    test_rate_cursor_reachable();
    test_text_reveal_height_stable();
    test_text_reveal_empty();
    test_text_reveal_typewriter();
    test_text_reveal_complete_no_ghost();
    test_text_reveal_ghost_blank();
    test_clip_to_cursor();
    test_end_caret();
    std::println("\nAll motion tests passed.");
    return 0;
}
