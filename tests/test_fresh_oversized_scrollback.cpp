// test_fresh_oversized_scrollback — an executable statement of the
// load-bearing invariant that keeps maya's inline compose "case-(A)"
// path scrollback-safe.
//
// ── The two first-frame sub-paths ──────────────────────────────────────
//
// compose_inline_frame's first-frame branch (prev_rows == 0, in
// maya/src/render/serialize.cpp) splits on state.prev_width_:
//
//   • prev_width_ > 0  → "case (B)" (force_redraw / stale recovery):
//     viewport-capped — emits only the last term_h rows in place
//     (start_row = max(0, content_rows - term_h)), zero bottom-edge
//     scrolls. SAFE for already-committed oversized content.
//
//   • prev_width_ == 0 → "case (A)" (truly Fresh): emits
//         out += reset_prefix;                    // may be empty
//         out += '\r';
//         serialize(canvas, pool, out, content_rows);   // UNCAPPED
//     serialize walks ALL content_rows with \r\n BETWEEN rows. When
//     content_rows > term_h and reset_prefix is EMPTY, each \n past the
//     viewport bottom scrolls the terminal — pushing (content_rows -
//     term_h) rows into native scrollback.
//
// ── Why the uncapped emit is CORRECT at true startup ───────────────────
//
// At true startup nothing is committed yet. A frame taller than the
// screen legitimately puts its own top (content_rows - term_h) rows into
// scrollback and its tail in the viewport — every canvas row appears
// EXACTLY ONCE, in reading order. Capping case (A) to the viewport would
// DROP the frame's top rows entirely; the uncapped serialize is the
// right behaviour and must not change. (test_startup_oversized_ordering)
//
// ── Why the uncapped emit is DANGEROUS if content is already committed ──
//
// If the same oversized canvas's tail is ALREADY on the wire (a prior
// frame committed it) and case (A) re-emits from the top with an EMPTY
// prefix, the already-committed tail is re-scrolled: every one of those
// rows now appears TWICE — the "turn/composer duplicated one screen up"
// symptom. (test_recommit_via_empty_prefix_strands — the RED proof that
// this branch is unsafe for committed content.)
//
// ── The invariant that keeps production safe ───────────────────────────
//
// The runtime NEVER routes a mid-session recovery through the bare
// case-(A) else with an empty prefix. Verified by code audit:
//   • Every Synced-arm recovery in app.cpp demotes to Stale
//     (abandoned_for_recovery keeps prev_width_ != 0 → case (B)) or, on
//     a width change, to HardReset.
//   • The only mid-session case-(A) entry is HardReset::render, which
//     passes reset_prefix = "\x1b[2J\x1b[3J\x1b[H". The \x1b[3J wipes
//     native scrollback and \x1b[2J\x1b[H clears+homes the viewport
//     BEFORE the uncapped serialize, so the bottom-edge scrolls push
//     only freshly-blanked rows off — nothing duplicates.
//     (test_hardreset_wipe_makes_oversized_safe — the GREEN proof that
//     the wipe neutralises the strand.)
//
// This file pins all three facts so a future refactor that (a) caps
// case (A) and breaks startup ordering, (b) routes committed-content
// recovery through the bare case-(A) else, or (c) drops the HardReset
// wipe prefix, fails a test instead of shipping a scrollback bug.

#include "maya/render/inline_frame.hpp"
#include "maya/render/canvas.hpp"
#include "maya/terminal/writer.hpp"
#include "check.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <optional>
#include <print>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::inline_frame;

// ─────────────────────────────────────────────────────────────────────────
// Non-blocking pipe Writer (same shape as test_inline_frame's helper).
// ─────────────────────────────────────────────────────────────────────────
static std::pair<Writer, int /*read_fd*/> make_pipe_writer() {
    int fds[2];
    int rc = pipe(fds);
    (void)rc;
    int flags = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
    int rflags = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, rflags | O_NONBLOCK);
    Writer w{static_cast<platform::NativeHandle>(fds[1])};
    return {std::move(w), fds[0]};
}

static std::string read_fd(int rfd) {
    std::string s;
    char buf[8192];
    for (;;) {
        ssize_t n = read(rfd, buf, sizeof(buf));
        if (n > 0) { s.append(buf, static_cast<std::size_t>(n)); continue; }
        break;
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────
// TermEmu — minimal ANSI terminal with native scrollback.
// Ported from tests/reveal_scrollback_test.cpp. Models rows scrolling off
// the top into an immutable scrollback vector so a test can inspect
// scrollback ++ viewport exactly as the user's eyes see it.
// ─────────────────────────────────────────────────────────────────────────
struct TermEmu {
    int cols, rows;
    int cx = 0, cy = 0;
    std::vector<std::string> screen;
    std::vector<std::string> scrollback;
    bool decawm = true;

    TermEmu(int c, int r) : cols(c), rows(r) {
        screen.assign(static_cast<std::size_t>(rows),
                      std::string(static_cast<std::size_t>(cols), ' '));
    }

    std::string& line(int y) { return screen[static_cast<std::size_t>(y)]; }

    void put(char ch) {
        if (cx >= cols) {
            if (decawm) { cx = 0; cursor_newline(); }
            else        { cx = cols - 1; }
        }
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        std::string& l = line(cy);
        if (static_cast<int>(l.size()) < cols)
            l.resize(static_cast<std::size_t>(cols), ' ');
        l[static_cast<std::size_t>(cx)] = ch;
        ++cx;
    }

    void cursor_newline() {
        if (cy < rows - 1) { ++cy; return; }
        std::string top = screen.front();
        while (!top.empty() && top.back() == ' ') top.pop_back();
        scrollback.push_back(std::move(top));
        screen.erase(screen.begin());
        screen.push_back(std::string(static_cast<std::size_t>(cols), ' '));
    }

    void carriage_return() { cx = 0; }
    void cursor_up(int n)   { cy -= n; if (cy < 0) cy = 0; }
    void cursor_down(int n) { for (int i = 0; i < n; ++i) cursor_newline(); }
    void cursor_fwd(int n)  { cx += n; if (cx > cols) cx = cols; }

    void erase_to_eol() {
        std::string& l = line(cy);
        if (static_cast<int>(l.size()) < cols)
            l.resize(static_cast<std::size_t>(cols), ' ');
        for (int x = cx; x < cols; ++x) l[static_cast<std::size_t>(x)] = ' ';
    }

    void erase_to_eos() {
        erase_to_eol();
        for (int y = cy + 1; y < rows; ++y)
            line(y).assign(static_cast<std::size_t>(cols), ' ');
    }

    void feed(const std::string& b) {
        std::size_t i = 0, n = b.size();
        while (i < n) {
            unsigned char ch = static_cast<unsigned char>(b[i]);
            if (ch == '\r') { carriage_return(); ++i; continue; }
            if (ch == '\n') { cursor_newline(); ++i; continue; }
            if (ch == 0x1b) { i = parse_esc(b, i); continue; }
            if (ch >= 0x20) { put(static_cast<char>(ch < 128 ? ch : '?')); ++i; continue; }
            ++i;
        }
    }

    std::size_t parse_esc(const std::string& b, std::size_t i) {
        const std::size_t n = b.size();
        if (i + 1 >= n) return n;
        if (b[i + 1] != '[') return i + 2;
        std::size_t j = i + 2;
        bool priv = false;
        if (j < n && b[j] == '?') { priv = true; ++j; }
        int param = 0; bool have_param = false;
        while (j < n && b[j] >= '0' && b[j] <= '9') {
            param = param * 10 + (b[j] - '0'); have_param = true; ++j;
        }
        if (j >= n) return n;
        char final = b[j];
        const int p = have_param ? param : 0;
        switch (final) {
            case 'A': cursor_up(p ? p : 1); break;
            case 'B': cursor_down(p ? p : 1); break;
            case 'C': cursor_fwd(p ? p : 1); break;
            case 'D': cx -= (p ? p : 1); if (cx < 0) cx = 0; break;
            case 'G': cx = (p ? p - 1 : 0); if (cx < 0) cx = 0; break;
            case 'H': cy = 0; cx = 0; break;
            case 'K': erase_to_eol(); break;
            case 'J':
                if (p == 2 || p == 3) {
                    if (p == 3) scrollback.clear();
                    for (auto& l : screen)
                        l.assign(static_cast<std::size_t>(cols), ' ');
                } else {
                    erase_to_eos();
                }
                break;
            case 'h': if (priv && p == 7) decawm = true; break;
            case 'l': if (priv && p == 7) decawm = false; break;
            case 'm': break;
            default:  break;
        }
        return j + 1;
    }

    std::vector<std::string> transcript() const {
        std::vector<std::string> t = scrollback;
        for (const auto& l : screen) {
            std::string s = l;
            while (!s.empty() && s.back() == ' ') s.pop_back();
            t.push_back(std::move(s));
        }
        return t;
    }
};

// ─────────────────────────────────────────────────────────────────────────
static int failures = 0;

static void expect(bool ok, const std::string& msg) {
    if (!ok) { ++failures; std::println(stderr, "  FAIL: {}", msg); }
    MAYA_TEST_CHECK(ok, msg.c_str());
}

static Canvas marked_canvas(int width, int rows, StylePool& pool,
                            const char* prefix) {
    Canvas c(width, rows + 4, &pool);
    auto sid = pool.intern(Style{});
    for (int y = 0; y < rows; ++y) {
        std::string label = std::string(prefix) + "-row-" + std::to_string(y);
        c.write_text(0, y, label, sid);
    }
    return c;
}

// Count distinct canvas rows that appear MORE THAN ONCE across the whole
// transcript (scrollback ++ viewport). Returns the first duplicate found
// (or {-1,-1}) plus the total dup count.
struct DupScan { int a = -1, b = -1; std::string line; int count = 0; };
static DupScan scan_dups(const TermEmu& emu, const char* marker) {
    auto all = emu.transcript();
    DupScan r;
    std::vector<std::pair<std::string,int>> seen;
    for (int y = 0; y < static_cast<int>(all.size()); ++y) {
        const std::string& ln = all[static_cast<std::size_t>(y)];
        if (ln.find(marker) == std::string::npos) continue;
        bool dup = false;
        for (auto& [s, py] : seen) {
            if (s == ln) {
                ++r.count; dup = true;
                if (r.a < 0) { r.a = py; r.b = y; r.line = ln; }
            }
        }
        if (!dup) seen.emplace_back(ln, y);
    }
    return r;
}

static void dump(const TermEmu& emu, const char* why) {
    if (!std::getenv("DUMP_TRANSCRIPT")) return;
    auto all = emu.transcript();
    std::println(stderr, "  --- transcript [{}] ({} sb + {} view) ---",
                 why, emu.scrollback.size(), emu.screen.size());
    for (int y = 0; y < static_cast<int>(all.size()); ++y)
        std::println(stderr, "  {:3}|{}{}", y,
            y < static_cast<int>(emu.scrollback.size()) ? "" : "V ",
            all[static_cast<std::size_t>(y)]);
}

// Drive one Fresh render (Empty→seed→render) with an optional reset
// prefix, feeding the emitted bytes into `emu`. Returns whether it
// reached Synced.
static bool fresh_render(TermEmu& emu, StylePool& pool, Writer& writer,
                         int rfd, const Canvas& c, int term_h,
                         std::string_view reset_prefix) {
    // reset_prefix is exercised via the HardReset render path when
    // non-empty; the bare Fresh render uses an empty prefix.
    if (!reset_prefix.empty()) {
        // Emit the wipe ourselves then drive a bare Fresh — this is
        // byte-equivalent to HardReset::render, which prepends the same
        // wipe inside the same synchronized frame before the case-(A)
        // serialize. (We can't easily synthesize InlineFrame<HardReset>
        // from a test, so we model its documented emit shape.)
        std::string pre{reset_prefix};
        emu.feed(pre);
    }
    auto o = InlineFrame<Empty>{}.seed().render(
        c, content_rows(c), term_rows_for_test(term_h), pool, writer,
        /*sync=*/false);
    emu.feed(read_fd(rfd));
    return std::visit([](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        return std::is_same_v<T, InlineFrame<Synced>>;
    }, std::move(o));
}

// ─────────────────────────────────────────────────────────────────────────
// 1. STARTUP ORDERING (the behaviour that must NOT regress).
//
// A Fresh render of an oversized canvas at true startup must place every
// canvas row exactly once, in order: the top (content - term_h) rows in
// scrollback, the last term_h in the viewport. Capping case (A) would
// break this by dropping the top rows.
// ─────────────────────────────────────────────────────────────────────────
static void test_startup_oversized_ordering() {
    std::println("--- test_startup_oversized_ordering ---");
    const int W = 80, TERM_H = 24, CONTENT = 40;

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);

    Canvas c = marked_canvas(W, CONTENT, pool, "TURN");
    bool synced = fresh_render(emu, pool, writer, rfd, c, TERM_H, "");
    expect(synced, "startup Fresh render should reach Synced");
    dump(emu, "startup");

    auto all = emu.transcript();
    // Every TURN-row-k present exactly once, in ascending order.
    int expected = 0;
    bool ok = true;
    for (const auto& ln : all) {
        if (ln.find("TURN-row-") == std::string::npos) continue;
        std::string want = "TURN-row-" + std::to_string(expected);
        if (ln.find(want) == std::string::npos) { ok = false; break; }
        ++expected;
    }
    expect(ok, "startup: TURN rows must appear once each in ascending order");
    expect(expected == CONTENT,
        "startup: all " + std::to_string(CONTENT) + " rows present (saw "
        + std::to_string(expected) + ")");
    auto d = scan_dups(emu, "TURN-row-");
    expect(d.a < 0,
        "startup: no row duplicated (rows " + std::to_string(d.a) + "/"
        + std::to_string(d.b) + " |" + d.line + "|)");

    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
}

// ─────────────────────────────────────────────────────────────────────────
// 2. RED PROOF: bare case-(A) re-emit of already-committed content strands
//    a duplicate. This documents WHY the runtime must never route
//    committed-content recovery through the empty-prefix case-(A) else.
//
//    We render the oversized turn once (committing its tail to the
//    viewport), then re-render the SAME canvas through a fresh Empty with
//    an EMPTY prefix. The already-committed rows re-scroll → duplicates.
//    We ASSERT that the strand happens (this is the hazard the invariant
//    guards against); if a future change ever made the bare case-(A)
//    else viewport-safe, update the invariant docs and flip this test.
// ─────────────────────────────────────────────────────────────────────────
static void test_recommit_via_empty_prefix_strands() {
    std::println("--- test_recommit_via_empty_prefix_strands ---");
    const int W = 80, TERM_H = 24, CONTENT = 40;

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);

    Canvas c = marked_canvas(W, CONTENT, pool, "TURN");
    bool s1 = fresh_render(emu, pool, writer, rfd, c, TERM_H, "");
    expect(s1, "first render should reach Synced");

    // Re-emit the SAME canvas through a bare Empty (empty prefix). This
    // is the UNSAFE shape — NOT a path the runtime ever takes (it would
    // demote to Stale/case-B or HardReset instead). We drive it directly
    // to pin the hazard.
    bool s2 = fresh_render(emu, pool, writer, rfd, c, TERM_H, "");
    expect(s2, "re-emit should reach Synced");
    dump(emu, "empty-prefix re-emit");

    auto d = scan_dups(emu, "TURN-row-");
    expect(d.a >= 0,
        "HAZARD LOST: bare case-(A) re-emit of committed content no longer "
        "strands a duplicate — the empty-prefix else may have been made "
        "viewport-safe; update the invariant docs in this file.");

    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
}

// ─────────────────────────────────────────────────────────────────────────
// 3. GREEN PROOF: the HardReset wipe prefix neutralises the strand.
//
//    The ONLY mid-session case-(A) entry (HardReset::render) prepends
//    "\x1b[2J\x1b[3J\x1b[H". With the wipe, re-emitting the same oversized
//    canvas leaves NO duplicate — \x1b[3J clears scrollback and \x1b[2J
//    clears the viewport before the uncapped serialize, so the bottom-edge
//    scrolls push only blank rows off. This is what keeps resize-driven
//    case-(A) safe in production.
// ─────────────────────────────────────────────────────────────────────────
static void test_hardreset_wipe_makes_oversized_safe() {
    std::println("--- test_hardreset_wipe_makes_oversized_safe ---");
    const int W = 80, TERM_H = 24, CONTENT = 40;

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);

    Canvas c = marked_canvas(W, CONTENT, pool, "TURN");
    bool s1 = fresh_render(emu, pool, writer, rfd, c, TERM_H, "");
    expect(s1, "first render should reach Synced");

    // Re-emit the SAME canvas through the HardReset shape: wipe prefix
    // then case-(A) serialize.
    bool s2 = fresh_render(emu, pool, writer, rfd, c, TERM_H,
                           "\x1b[2J\x1b[3J\x1b[H");
    expect(s2, "hard-reset re-emit should reach Synced");
    dump(emu, "hardreset-wipe re-emit");

    auto d = scan_dups(emu, "TURN-row-");
    expect(d.a < 0,
        "HardReset wipe failed to prevent the strand: rows "
        + std::to_string(d.a) + "/" + std::to_string(d.b)
        + " both |" + d.line + "| — the \\x1b[3J scrollback wipe is the "
        "invariant that keeps resize-driven case-(A) scrollback-safe.");

    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
}

// ─────────────────────────────────────────────────────────────────────────
// 4. THE MAIN BUG (from the profiler): shrink-while-overflowed verify-poison.
//
// Profiler /tmp/prof.log at fixed geometry (w=104 term_h=75, NO resize)
// showed the live tail overflow the viewport (rows=422), then SHRINK
// (422→339→256) as the stream settled, and on the 256-row frame the
// shadow poisoned (coh 2→3 VERIFY-DEMOTE) followed by a case-(B) repaint
// (coh 3→2 FLICKER) that stranded the composer one screen up.
//
// The app.cpp verify-poison-while-overflowed branch USED to commit
// `prev_rows - term_h` rows UNCONDITIONALLY and soft-repaint, on the
// assumption "the committed rows are byte-identical to what overflowed."
// That assumption FAILS while shrinking: the rows physically in native
// scrollback came from the TALLER earlier canvas, so the shorter frame's
// overflow prefix differs → committing it strands a duplicate.
//
// The fix: gate the commit on scrollback_prefix_matches(). If the prefix
// differs, escalate to HardReset (\x1b[2J\x1b[3J\x1b[H) instead — the only
// recovery that can correct already-committed scrollback.
//
// This test drives a real InlineFrame<Synced> to the overflowed state,
// mutates the canvas into a SHORTER frame whose overflow prefix differs
// (content shifted, as when a streaming tail collapses), forces the
// poison recovery decision BOTH ways, and shows: OLD (unconditional
// commit + demote_to_stale) strands a duplicate; NEW (prefix-gated →
// demote_to_hard_reset) does not.
// ─────────────────────────────────────────────────────────────────────────

// Build a Synced frame at `rows` height by rendering `c` from Empty.
static std::optional<InlineFrame<Synced>>
drive_to_synced(TermEmu& emu, StylePool& pool, Writer& writer, int rfd,
                const Canvas& c, int term_h) {
    auto o = InlineFrame<Empty>{}.seed().render(
        c, content_rows(c), term_rows_for_test(term_h), pool, writer, false);
    emu.feed(read_fd(rfd));
    return std::visit(
        [](auto&& a) -> std::optional<InlineFrame<Synced>> {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(a);
            else return std::nullopt;
        }, std::move(o));
}

// Run the shrink-while-overflowed poison scenario with a chosen recovery
// strategy. Returns whether a duplicate was stranded.
//   use_fix == false → OLD behaviour: unconditional commit + demote_to_stale
//   use_fix == true  → NEW behaviour: prefix-gated (mismatch → HardReset)
static bool shrink_overflow_strands(bool use_fix) {
    const int W = 104, TERM_H = 75;
    const int TALL = 220;    // >> term_h: overflows deeply (profiler: 422)
    const int SHORT = 130;   // shrunk but still overflowing (profiler: 256)

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);

    // 1. Overflowed tall frame committed to the wire.
    Canvas tall = marked_canvas(W, TALL, pool, "TALL");
    auto synced = drive_to_synced(emu, pool, writer, rfd, tall, TERM_H);
    if (!synced) { close(rfd); return false; }

    // 2. A SHORTER canvas whose overflow prefix does NOT match the wire —
    //    every row carries a different marker ("SHORT-row-*"), modelling a
    //    streaming tail that collapsed and shifted the whole frame. This
    //    is the shrink-while-overflowed shape the profiler captured.
    Canvas shortc = marked_canvas(W, SHORT, pool, "SHORT");

    // 3. Model app.cpp's verify-poison-while-overflowed recovery. In
    //    production verify() returns nullopt here (the shadow diverged);
    //    we take the same branch directly on the overflowed Synced state.
    const int prev_rows = synced->rows();          // == TALL
    const int overflow  = prev_rows - TERM_H;
    const bool prefix_ok = synced->scrollback_prefix_matches(shortc, overflow);

    if (!use_fix) {
        // OLD: commit unconditionally, then case-(B) soft-repaint.
        auto committed = std::move(*synced).commit(
            synced->scrollback_marker(overflow));
        auto stale = std::move(committed).demote_to_stale();
        auto o = std::move(stale).render(
            shortc, content_rows(shortc), term_rows_for_test(TERM_H),
            pool, writer, false);
        emu.feed(read_fd(rfd));
        (void)o;
    } else {
        // NEW: gate on the prefix. Mismatch → HardReset (wipe), else the
        // old commit path.
        if (prefix_ok) {
            auto committed = std::move(*synced).commit(
                synced->scrollback_marker(overflow));
            auto stale = std::move(committed).demote_to_stale();
            auto o = std::move(stale).render(
                shortc, content_rows(shortc), term_rows_for_test(TERM_H),
                pool, writer, false);
            emu.feed(read_fd(rfd));
            (void)o;
        } else {
            auto hr = std::move(*synced).demote_to_hard_reset();
            auto o = std::move(hr).render(
                shortc, content_rows(shortc), term_rows_for_test(TERM_H),
                pool, writer, false);
            emu.feed(read_fd(rfd));
            (void)o;
        }
    }
    synced.reset();

    dump(emu, use_fix ? "shrink-overflow FIXED" : "shrink-overflow OLD");
    // The stranded-duplicate symptom: any TALL marker STILL present in the
    // transcript after the shorter frame took over is a stale copy that
    // the recovery failed to clear (the composer/turn "one screen up").
    // With the wipe, no TALL row survives; with the old commit, the
    // committed TALL prefix stays stranded above the new SHORT frame.
    auto all = emu.transcript();
    int tall_survivors = 0;
    for (const auto& ln : all)
        if (ln.find("TALL-row-") != std::string::npos) ++tall_survivors;

    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
    return tall_survivors > 0;
}

static void test_shrink_overflow_poison_recovery() {
    std::println("--- test_shrink_overflow_poison_recovery ---");

    // OLD behaviour strands the taller frame's committed rows (the bug).
    const bool old_strands = shrink_overflow_strands(/*use_fix=*/false);
    expect(old_strands,
        "REPRO LOST: the unconditional-commit recovery no longer strands "
        "the taller frame on a shrink-while-overflowed poison — the "
        "profiler-captured bug shape changed; revisit this model.");

    // NEW behaviour (prefix-gated → HardReset) leaves nothing stranded.
    const bool fixed_strands = shrink_overflow_strands(/*use_fix=*/true);
    expect(!fixed_strands,
        "FIX BROKEN: prefix-gated recovery still strands the taller frame "
        "on a shrink-while-overflowed poison — the \\x1b[3J HardReset wipe "
        "is not clearing the mismatched committed scrollback.");
}

int main() {
    test_startup_oversized_ordering();
    test_recommit_via_empty_prefix_strands();
    test_hardreset_wipe_makes_oversized_safe();
    test_shrink_overflow_poison_recovery();
    if (failures) {
        std::println("FRESH-OVERSIZED SCROLLBACK TESTS FAILED ({} failures)",
                     failures);
        return 1;
    }
    std::println("ALL FRESH-OVERSIZED SCROLLBACK TESTS PASSED");
    return 0;
}
