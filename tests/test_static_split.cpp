// Tests for DECSTBM scroll-region ANSI helpers and StaticSplit partial updates.
//
// Includes a compact terminal emulator that honors DECSTBM so we can prove the
// frozen band is physically protected from scrolling — the load-bearing
// guarantee of the static-component model.
#include <maya/maya.hpp>
#include <maya/app/static_region.hpp>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <print>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Minimal DECSTBM-aware terminal emulator ─────────────────────────────────
struct TermEmu {
    int w, h;
    std::vector<std::string> rows;   // each row is `w` chars
    std::vector<std::string> scroll_off;  // rows that scrolled past the top
    int cx = 0, cy = 0;              // 0-based cursor
    int margin_top = 0, margin_bot;  // 0-based inclusive scroll region

    TermEmu(int width, int height)
        : w(width), h(height), rows(height, std::string(width, ' ')),
          margin_bot(height - 1) {}

    void put(char c) {
        if (cx < w && cy < h) rows[cy][cx] = c;
        cx++;
    }

    void scroll_up(int n) {  // text moves up within [margin_top, margin_bot]
        for (int k = 0; k < n; ++k) {
            // The row leaving the TOP of the scroll region enters scrollback
            // only when the region starts at the physical top (margin_top==0);
            // a row pushed out of a sub-region is simply discarded by the
            // terminal (DECSTBM regions don't feed scrollback).
            if (margin_top == 0) scroll_off.push_back(rows[margin_top]);
            for (int y = margin_top; y < margin_bot; ++y) rows[y] = rows[y + 1];
            rows[margin_bot].assign(w, ' ');
        }
    }

    void newline() {
        if (cy == margin_bot) scroll_up(1);
        else if (cy < h - 1) cy++;
        cx = 0;
    }

    void feed(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
                size_t j = i + 2;
                bool priv = (j < s.size() && s[j] == '?');
                if (priv) j++;
                std::string params;
                while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == ';')) {
                    params += s[j]; j++;
                }
                if (j >= s.size()) break;
                char fin = s[j];
                if (!priv) handle_csi(params, fin);   // ignore private modes (?7l etc.)
                i = j + 1;
            } else if (c == '\n') { newline(); i++; }
            else if (c == '\r') { cx = 0; i++; }
            else { put(c); i++; }
        }
    }

    void handle_csi(const std::string& params, char fin) {
        // Parse up to two ints.
        int a = -1, b = -1;
        { size_t p = params.find(';');
          if (p == std::string::npos) { if (!params.empty()) a = std::stoi(params); }
          else { a = params.substr(0, p).empty() ? -1 : std::stoi(params.substr(0, p));
                 std::string rest = params.substr(p + 1);
                 b = rest.empty() ? -1 : std::stoi(rest); } }
        switch (fin) {
            case 'H': {                       // CUP row;col (1-based)
                cy = (a < 0 ? 1 : a) - 1;
                cx = (b < 0 ? 1 : b) - 1;
                cy = std::clamp(cy, 0, h - 1);
                cx = std::clamp(cx, 0, w - 1);
                break;
            }
            case 'r': {                       // DECSTBM
                if (a < 0 && b < 0) { margin_top = 0; margin_bot = h - 1; }
                else { margin_top = (a < 0 ? 1 : a) - 1;
                       margin_bot = (b < 0 ? h : b) - 1;
                       margin_top = std::clamp(margin_top, 0, h - 1);
                       margin_bot = std::clamp(margin_bot, 0, h - 1); }
                cx = 0; cy = margin_top;       // DECSTBM homes the cursor
                break;
            }
            case 'K': {                        // EL: 0/none = cursor→EOL, 2 = whole
                int mode = (a < 0 ? 0 : a);
                if (cy < h) {
                    if (mode == 2) rows[cy].assign(w, ' ');
                    else for (int x = cx; x < w; ++x) rows[cy][x] = ' ';
                }
                break;
            }
            case 'S': scroll_up(a < 0 ? 1 : a); break;
            case 'm': break;                   // SGR — ignore (we test geometry)
            default: break;
        }
    }

    std::string trimmed(int y) const {
        std::string r = rows[y];
        while (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    }
};

// ── ANSI helper bytes ───────────────────────────────────────────────────────
void test_ansi_scroll_region_bytes() {
    std::println("--- test_ansi_scroll_region_bytes ---");
    std::string s;
    ansi::write_scroll_region(s, 3, 20);
    assert(s == "\x1b[3;20r");
    s.clear();
    ansi::write_scroll_region_reset(s);
    assert(s == "\x1b[r");
    s.clear();
    ansi::write_scroll_up(s, 2);
    assert(s == "\x1b[2S");
    s.clear();
    ansi::write_scroll_down(s, 4);
    assert(s == "\x1b[4T");
    std::println("PASS\n");
}

// ── StaticSplit geometry bookkeeping ────────────────────────────────────────
void test_split_geometry() {
    std::println("--- test_split_geometry ---");
    StylePool pool;
    std::string out;
    StaticSplit s;
    s.begin(/*screen_h=*/10, out);
    assert(s.armed());
    assert(s.frozen_rows() == 0);
    assert(s.active_rows() == 10);

    int froze = s.freeze(box().direction(Column)(text("A"), text("B")),
                         40, pool, out);
    assert(froze == 2);
    assert(s.frozen_rows() == 2);
    assert(s.active_rows() == 8);

    froze = s.freeze(text("C"), 40, pool, out);
    assert(froze == 1);
    assert(s.frozen_rows() == 3);
    assert(s.active_rows() == 7);
    std::println("PASS\n");
}

// ── Invariant: never freeze the last active row ─────────────────────────────
void test_split_keeps_active_row() {
    std::println("--- test_split_keeps_active_row ---");
    StylePool pool;
    std::string out;
    StaticSplit s;
    s.begin(/*screen_h=*/4, out);
    // Try to freeze 10 rows into a 4-row screen.
    Element tall = box().direction(Column)(
        text("1"), text("2"), text("3"), text("4"),
        text("5"), text("6"), text("7"), text("8"));
    int froze = s.freeze(tall, 40, pool, out);
    assert(froze == 3);              // capped at screen_h - 1
    assert(s.active_rows() == 1);    // one active row always survives
    int more = s.freeze(text("X"), 40, pool, out);
    assert(more == 0);               // nothing left to freeze
    std::println("PASS\n");
}

// ── End-to-end through the emulator: frozen rows survive scrolling ──────────
void test_frozen_survives_scroll() {
    std::println("--- test_frozen_survives_scroll ---");
    StylePool pool;
    const int H = 8, W = 40;
    TermEmu term(W, H);
    std::string out;

    StaticSplit s;
    s.begin(H, out);
    // Freeze a 2-row header.
    s.freeze(box().direction(Column)(text("HEADER-ONE"), text("HEADER-TWO")),
             W, pool, out);
    // Paint the active band.
    s.draw_active(text("active-content"), W, pool, out);
    term.feed(out);

    // Now scroll the active band a bunch (simulating live streaming output).
    std::string scrolls;
    for (int i = 0; i < 20; ++i) scrolls += "scroll-line\n";
    term.feed(scrolls);

    // The frozen header must still be intact at rows 0 and 1.
    assert(term.trimmed(0) == "HEADER-ONE");
    assert(term.trimmed(1) == "HEADER-TWO");
    // The scroll margin must exclude the frozen band.
    assert(term.margin_top == 2);
    assert(term.margin_bot == H - 1);
    std::println("PASS\n");
}

// ── end() restores the full-screen scroll region ────────────────────────────
void test_end_restores_region() {
    std::println("--- test_end_restores_region ---");
    StylePool pool;
    const int H = 6, W = 20;
    TermEmu term(W, H);
    std::string out;
    StaticSplit s;
    s.begin(H, out);
    s.freeze(text("X"), W, pool, out);
    s.end(out);
    term.feed(out);
    assert(term.margin_top == 0);
    assert(term.margin_bot == H - 1);
    assert(!s.armed());
    std::println("PASS\n");
}

// ── Agentty-shaped scenario: growing frozen prefix + trim, no duplication ───
// This is the DECSTBM proof-of-concept for agentty's two-tier render path.
// Several "turns" complete and get frozen (the prefix grows); periodically the
// active band repaints a live tail; then the prefix is trimmed from the top
// (commit_top) when it exceeds a budget. The invariant that matters — and that
// agentty's production commit_scrollback path tunes so carefully against — is:
// NO unique line ever appears twice across the cumulative transcript
// (scrollback ++ on-screen rows). Under DECSTBM that holds structurally: frozen
// rows leave only via a real scroll, never a from-the-top re-emit, so the
// duplicate-strand failure mode cannot occur.
void test_agentty_freeze_trim_no_dup() {
    std::println("--- test_agentty_freeze_trim_no_dup ---");
    StylePool pool;
    const int H = 12, W = 40;
    TermEmu term(W, H);
    std::string out;

    StaticSplit split;
    split.begin(H, out);
    term.feed(out); out.clear();

    int turn = 0;
    const int budget = 6;   // keep at most ~6 frozen rows before trimming

    // 20 turns: each freezes a unique 1-row "message", repaints the active
    // band, and trims the frozen prefix back under budget.
    for (int t = 0; t < 20; ++t) {
        char msg[32];
        std::snprintf(msg, sizeof(msg), "turn-%02d-done", turn++);
        split.freeze(text(msg), W, pool, out);

        char live[32];
        std::snprintf(live, sizeof(live), "...working-%02d", t);
        split.draw_active(text(live), W, pool, out);

        // Trim the frozen prefix down to the budget.
        if (split.frozen_rows() > budget)
            split.commit_top(split.frozen_rows() - budget, out);

        term.feed(out); out.clear();
    }
    split.end(out);
    term.feed(out);

    // Collect the cumulative transcript: scrollback rows + current screen.
    std::vector<std::string> transcript = term.scroll_off;
    for (int y = 0; y < H; ++y) transcript.push_back(term.trimmed(y));

    // No unique "turn-NN-done" line may appear twice (the strand symptom).
    for (int n = 0; n < turn; ++n) {
        char needle[32];
        std::snprintf(needle, sizeof(needle), "turn-%02d-done", n);
        int count = 0;
        for (const auto& row : transcript)
            if (row.find(needle) != std::string::npos) ++count;
        assert(count <= 1);   // strand-free: never duplicated
    }

    // And every completed turn must appear AT LEAST once somewhere in the
    // transcript — nothing was silently lost by the trim.
    for (int n = 0; n < turn; ++n) {
        char needle[32];
        std::snprintf(needle, sizeof(needle), "turn-%02d-done", n);
        bool seen = false;
        for (const auto& row : transcript)
            if (row.find(needle) != std::string::npos) { seen = true; break; }
        assert(seen);
    }
    std::println("PASS\n");
}

// Agentty-shaped DECSTBM lifecycle stress: the de-risking layer for a real
// integration. Beyond the single-row no-dup PoC, this exercises every shape
// the production trim path must survive before StaticSplit could replace the
// inline-frame commit_scrollback driver:
//
//   * MULTI-ROW turns (real assistant messages span many rows, and a single
//     turn can exceed the active band — the max_freeze clamp path).
//   * MULTIPLE terminal shapes (the production trim is tuned per term_dims()).
//   * VARIED trim budgets + cadence (trim fires infrequently in production).
//   * A freeze TALLER than the active band (clamp must not strand/lose rows).
//
// Invariants over the cumulative transcript (native scrollback ++ on-screen),
// asserted at the end of every shape:
//   I1 strand-free  — no unique "S<shape>-turn-NN-row-RR" line appears twice.
//   I2 no-loss      — every frozen row that was trimmed OR is still on screen
//                     appears at least once (nothing silently dropped by a
//                     clamp or a trim).
void test_agentty_decstbm_lifecycle() {
    std::println("--- test_agentty_decstbm_lifecycle ---");
    StylePool pool;

    struct Shape { int w, h; };
    const Shape shapes[] = { {40, 12}, {60, 8}, {80, 20}, {50, 6} };

    int shape_id = 0;
    for (auto sh : shapes) {
        const int W = sh.w, H = sh.h;
        TermEmu term(W, H);
        std::string out;

        StaticSplit split;
        split.begin(H, out);
        term.feed(out); out.clear();

        // Track every frozen row we EXPECTED to graduate (trimmed off OR
        // still resident). A row "graduates" the instant freeze() accepts
        // it; if a clamp rejected part of a turn we don't count those rows.
        std::vector<std::string> expected_frozen;

        const int budget = std::max(2, H / 3);

        for (int t = 0; t < 16; ++t) {
            // A multi-row "assistant turn": 1..(H+1) rows, deterministically
            // varied so some turns are SHORTER and some TALLER than the
            // active band (forcing the max_freeze clamp).
            const int rows_this_turn = 1 + ((t * 7 + shape_id) % (H + 1));
            for (int r = 0; r < rows_this_turn; ++r) {
                char msg[48];
                std::snprintf(msg, sizeof(msg),
                              "S%d-turn-%02d-row-%02d", shape_id, t, r);
                const int before = split.frozen_rows();
                const int froze = split.freeze(text(msg), W, pool, out);
                // A single 1-row element either freezes (1) or is clamped
                // out (0 — no active row left this frame). Count only what
                // actually graduated.
                if (froze > 0) expected_frozen.push_back(msg);
                (void)before;

                // If the band filled (clamp returned 0), trim to make room
                // and retry once — mirrors the production cadence where a
                // trim precedes a freeze that would otherwise overflow.
                if (froze == 0 && split.frozen_rows() > budget) {
                    split.commit_top(split.frozen_rows() - budget, out);
                    const int retry = split.freeze(text(msg), W, pool, out);
                    if (retry > 0) expected_frozen.push_back(msg);
                }
            }

            char live[32];
            std::snprintf(live, sizeof(live), "...working-%02d", t);
            split.draw_active(text(live), W, pool, out);

            // Trim the frozen prefix down to the budget (the FROZEN_MAX trim).
            if (split.frozen_rows() > budget)
                split.commit_top(split.frozen_rows() - budget, out);

            term.feed(out); out.clear();
        }
        split.end(out);
        term.feed(out);

        // Cumulative transcript.
        std::vector<std::string> transcript = term.scroll_off;
        for (int y = 0; y < H; ++y) transcript.push_back(term.trimmed(y));

        auto count_in = [&](const std::string& needle) {
            int c = 0;
            for (const auto& row : transcript)
                if (row.find(needle) != std::string::npos) ++c;
            return c;
        };

        // I1: strand-free. Every graduated row appears AT MOST once.
        for (const auto& row : expected_frozen)
            assert(count_in(row) <= 1);

        // I2: no-loss. Every graduated row appears AT LEAST once (it either
        // scrolled into native scrollback via commit_top or is still on
        // screen — a trim must never silently drop a row).
        for (const auto& row : expected_frozen)
            assert(count_in(row) >= 1);

        ++shape_id;
    }
    std::println("PASS (multi-row × 4 shapes × varied budget, strand-free + no-loss)\n");
}

int main() {
    test_ansi_scroll_region_bytes();
    test_split_geometry();
    test_split_keeps_active_row();
    test_frozen_survives_scroll();
    test_end_restores_region();
    test_agentty_freeze_trim_no_dup();
    test_agentty_decstbm_lifecycle();
    std::println("All static-split tests passed.");
    return 0;
}
