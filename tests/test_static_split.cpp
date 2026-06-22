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
#include <print>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Minimal DECSTBM-aware terminal emulator ─────────────────────────────────
struct TermEmu {
    int w, h;
    std::vector<std::string> rows;   // each row is `w` chars
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

int main() {
    test_ansi_scroll_region_bytes();
    test_split_geometry();
    test_split_keeps_active_row();
    test_frozen_survives_scroll();
    test_end_restores_region();
    std::println("All static-split tests passed.");
    return 0;
}
