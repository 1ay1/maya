// Tests for the DECSTBM scroll-region ANSI helpers.
//
// Includes a compact terminal emulator that honors DECSTBM so we can prove the
// frozen band is physically protected from scrolling — the load-bearing
// guarantee of the static-component model.
#include <maya/maya.hpp>
#include <algorithm>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
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
TEST_CASE("ansi scroll region bytes") {
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
