// tests/test_strata.cpp — The depositional-invariant proof for maya::strata.
//
// ─────────────────────────────────────────────────────────────────────────
// WHAT THIS PROVES
//
// Strata's whole claim (docs/internals/strata-renderer.md) is:
//
//   "Every emitted row appears EXACTLY ONCE across (native scrollback ∪
//    on-screen viewport), no matter how the session is fuzzed —
//    streaming append, width resize, height resize, wide-char content,
//    wholesale thread swap, or transcript rewind."
//
// A renderer that's "proven once by hand" is impressive on paper. This
// file makes the proof ADVERSARIAL and REPRODUCIBLE: a deterministic
// seeded fuzzer drives Strata through interleaved hostile operations,
// a VT terminal model consumes the REAL bytes Strata emits, and after
// every frame we assert the deposition invariant directly against the
// model's (scrollback ∪ screen). On failure the harness SHRINKS the
// operation log to a minimal reproducer and prints it.
//
// ─────────────────────────────────────────────────────────────────────────
// THE TERMINAL MODEL (vt::Term)
//
// Strata only ever emits a small, well-defined byte vocabulary (see
// inline_frame.hpp): CR / LF, cursor up/down (CUU/CUD), absolute column
// (CHA), erase-to-eol (EL), erase-screen + erase-saved-lines + home
// (\x1b[2J\x1b[3J\x1b[H), SGR (ignored for geometry), and printable runs.
// vt::Term implements exactly that subset over a continuous-buffer model:
// a vector of rows that NEVER shrinks (scrolled-off rows persist as
// immutable scrollback, exactly like a real terminal's saved-lines),
// plus a viewport window of the last `term_rows` rows. The cursor walks
// inside the viewport; writing past the bottom scrolls (appends a fresh
// row, advancing the scrollback boundary).
//
// This is the SECOND, INDEPENDENT party that Strata's design removed from
// the renderer — re-introduced HERE, in the test only, as the oracle.
// ─────────────────────────────────────────────────────────────────────────

#include "maya/render/strata.hpp"
#include "maya/render/canvas.hpp"
#include "maya/render/serialize.hpp"
#include "maya/terminal/writer.hpp"
#include "maya/element/element.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/text/unicode_width.hpp"
#include "check.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define CHECK(cond) MAYA_TEST_CHECK((cond), "")
#define CHECKM(cond, msg) MAYA_TEST_CHECK((cond), (msg))

using namespace maya;
using namespace maya::strata;

// ═════════════════════════════════════════════════════════════════════════
//  vt::Term — the oracle terminal model
// ═════════════════════════════════════════════════════════════════════════
namespace vt {

// A single screen cell — we only need the visible glyph for identity.
// Wide chars occupy two cells: the lead carries the codepoint, the trail
// is marked continuation (ch == TRAIL).
struct Cell {
    char32_t ch = U' ';
};
constexpr char32_t TRAIL = 0xFFFFFFFE;  // wide-char trailing half marker

class Term {
public:
    explicit Term(int cols, int rows) : cols_(cols), rows_(rows) {
        screen_.assign(rows_, std::vector<Cell>(cols_));
        cur_row_ = 0;
        cur_col_ = 0;
    }

    // A height-only resize keeps scrollback + visible content. A real
    // terminal anchors the bottom row; we grow/shrink the screen from the
    // TOP (rows leaving the top on a shrink go to scrollback; rows added on
    // a grow come blank at the top). Width change is handled by Strata as a
    // HardReset wipe, so we just adopt the new width here.
    void resize(int cols, int rows) {
        if (cols != cols_) {
            cols_ = cols;
            for (auto& r : scrollback_) r.resize(cols_);
            for (auto& r : screen_)     r.resize(cols_);
        }
        if (rows != rows_) {
            if (rows < rows_) {
                // xterm-faithful shrink: the terminal drops TRAILING BLANK
                // rows first (the empty space below the last content / the
                // cursor), and only scrolls content into history if the
                // occupied region still exceeds the new height. Find the
                // last occupied row (>= cursor, so the cursor line is kept).
                int last_occupied = cur_row_;
                for (int y = 0; y < rows_; ++y)
                    if (!row_blank(screen_[y])) last_occupied = std::max(last_occupied, y);
                int occupied = last_occupied + 1;          // rows [0, occupied)
                int drop = std::max(0, occupied - rows);    // overflow off the top
                for (int i = 0; i < drop; ++i)
                    scrollback_.push_back(std::move(screen_[i]));
                // New screen = the kept window [drop, drop+rows), padded
                // with blanks at the bottom if occupied < rows.
                std::vector<std::vector<Cell>> ns;
                ns.reserve(rows);
                for (int i = drop; i < drop + rows; ++i) {
                    if (i < rows_) ns.push_back(std::move(screen_[i]));
                    else           ns.emplace_back(cols_);
                }
                screen_ = std::move(ns);
                cur_row_ = std::clamp(cur_row_ - drop, 0, rows - 1);
            } else {
                // xterm-faithful grow: the terminal pulls rows BACK from the
                // saved-lines buffer to fill the new space at the top, and
                // only adds blank rows at the bottom once history is
                // exhausted. (A shrink-then-grow round-trips losslessly.)
                int add = rows - rows_;
                int pull = std::min<int>(add, static_cast<int>(scrollback_.size()));
                std::vector<std::vector<Cell>> ns;
                ns.reserve(rows);
                for (int i = static_cast<int>(scrollback_.size()) - pull;
                     i < static_cast<int>(scrollback_.size()); ++i)
                    ns.push_back(std::move(scrollback_[i]));
                scrollback_.resize(scrollback_.size() - pull);
                for (auto& r : screen_) ns.push_back(std::move(r));
                while (static_cast<int>(ns.size()) < rows) ns.emplace_back(cols_);
                screen_ = std::move(ns);
                cur_row_ += pull;   // content shifted down by the pulled rows
            }
            rows_ = rows;
        }
        clamp_cursor();
    }

    static bool row_blank(const std::vector<Cell>& r) {
        for (const auto& c : r) if (c.ch != U' ' && c.ch != TRAIL) return false;
        return true;
    }

    // Feed raw bytes emitted by Strata.
    void feed(std::string_view bytes) {
        // Invariant: the live screen is ALWAYS exactly rows_ rows. A drift
        // here means a resize/scroll path is wrong in the MODEL, not Strata.
        CHECK(static_cast<int>(screen_.size()) == rows_);
        size_t i = 0;
        while (i < bytes.size()) {
            unsigned char c = static_cast<unsigned char>(bytes[i]);
            if (c == 0x1b) { i = handle_escape(bytes, i); continue; }
            if (c == '\r') { cur_col_ = 0; ++i; continue; }
            if (c == '\n') { line_feed(); ++i; continue; }
            if (c == 0x07) { ++i; continue; }                 // BEL — ignore
            if (c < 0x20)  { ++i; continue; }                 // other C0 — ignore
            i = put_utf8(bytes, i);
        }
    }

    // ── Oracle queries ───────────────────────────────────────────────────
    // The continuous buffer = scrollback (immutable history) followed by the
    // live screen. row_text(y) indexes into that concatenation.
    int total_rows() const {
        return static_cast<int>(scrollback_.size() + screen_.size());
    }
    int scroll_base() const { return static_cast<int>(scrollback_.size()); }

    std::string row_text(int y) const {
        const std::vector<Cell>* r = nullptr;
        int sb = static_cast<int>(scrollback_.size());
        if (y < sb) r = &scrollback_[y];
        else        r = &screen_[y - sb];
        std::string out;
        int last = -1;
        for (int x = 0; x < static_cast<int>(r->size()); ++x)
            if ((*r)[x].ch != U' ' && (*r)[x].ch != TRAIL) last = x;
        for (int x = 0; x <= last; ++x) {
            if ((*r)[x].ch == TRAIL) continue;
            append_utf8(out, (*r)[x].ch);
        }
        return out;
    }

private:
    int cols_, rows_;
    std::vector<std::vector<Cell>> scrollback_;  // immutable history
    std::vector<std::vector<Cell>> screen_;      // exactly rows_ live rows
    int cur_row_ = 0;   // 0..rows_-1, relative to screen top
    int cur_col_ = 0;

    void clamp_cursor() {
        if (cur_row_ < 0) cur_row_ = 0;
        if (cur_row_ >= rows_) cur_row_ = rows_ - 1;
        if (cur_col_ < 0) cur_col_ = 0;
        if (cur_col_ >= cols_) cur_col_ = cols_ - 1;
    }

    // Move down one screen row; if at the bottom, scroll: screen[0] goes to
    // scrollback and a blank row appears at the bottom.
    void line_feed() {
        if (cur_row_ + 1 >= rows_) {
            scrollback_.push_back(std::move(screen_.front()));
            screen_.erase(screen_.begin());
            screen_.emplace_back(cols_);
            cur_row_ = rows_ - 1;
        } else {
            ++cur_row_;
        }
    }

    size_t put_utf8(std::string_view b, size_t i) {
        char32_t cp = 0; int len = 1;
        unsigned char c = static_cast<unsigned char>(b[i]);
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6 && i + 1 < b.size()) {
            cp = (c & 0x1f) << 6 | (b[i+1] & 0x3f); len = 2;
        } else if ((c >> 4) == 0xe && i + 2 < b.size()) {
            cp = (c & 0x0f) << 12 | (b[i+1] & 0x3f) << 6 | (b[i+2] & 0x3f); len = 3;
        } else if ((c >> 3) == 0x1e && i + 3 < b.size()) {
            cp = (c & 0x07) << 18 | (b[i+1] & 0x3f) << 12
               | (b[i+2] & 0x3f) << 6 | (b[i+3] & 0x3f); len = 4;
        } else { return i + 1; }  // malformed — skip one byte

        int w = unicode::char_width(cp);
        if (w <= 0) return i + len;  // zero-width — ignore for geometry
        if (cur_col_ + w > cols_) {
            // Auto-wrap: next line, col 0.
            line_feed();
            cur_col_ = 0;
        }
        screen_[cur_row_][cur_col_].ch = cp;
        if (w == 2 && cur_col_ + 1 < cols_)
            screen_[cur_row_][cur_col_ + 1].ch = TRAIL;
        cur_col_ += w;
        return i + len;
    }

    // Parse one CSI/escape, apply geometry effect, return new index.
    size_t handle_escape(std::string_view b, size_t i) {
        // i points at ESC.
        if (i + 1 >= b.size()) return b.size();
        char n = b[i + 1];
        if (n == '7' || n == '8') return i + 2;  // DECSC/DECRC — ignore
        if (n != '[') {
            // OSC (ESC ]) — skip to BEL or ST.
            if (n == ']') {
                size_t j = i + 2;
                while (j < b.size() && b[j] != 0x07
                       && !(b[j] == 0x1b && j + 1 < b.size() && b[j+1] == '\\'))
                    ++j;
                if (j < b.size() && b[j] == 0x07) return j + 1;
                if (j + 1 < b.size()) return j + 2;
                return b.size();
            }
            return i + 2;  // other 2-byte escape
        }
        // CSI: ESC [ params... final
        size_t j = i + 2;
        std::string params;
        while (j < b.size() && (b[j] == ';' || (b[j] >= '0' && b[j] <= '9')
                                || b[j] == '?')) {
            params += b[j];
            ++j;
        }
        if (j >= b.size()) return b.size();
        char fin = b[j];
        apply_csi(params, fin);
        return j + 1;
    }

    static int param_at(const std::string& p, int idx, int dflt) {
        // split p on ';', return idx-th as int (dflt if absent/empty)
        int cur = 0; int n = 0; bool any = false; int val = 0;
        for (char c : p) {
            if (c == ';') { if (cur == idx) return any ? val : dflt; ++cur; any = false; val = 0; }
            else if (c >= '0' && c <= '9') { any = true; val = val * 10 + (c - '0'); }
            ++n;
        }
        if (cur == idx) return any ? val : dflt;
        return dflt;
    }

    void apply_csi(const std::string& p, char fin) {
        switch (fin) {
        case 'A': cur_row_ = std::max(0, cur_row_ - std::max(1, param_at(p,0,1))); break;
        case 'B': cur_row_ = std::min(rows_ - 1, cur_row_ + std::max(1, param_at(p,0,1))); break;
        case 'C': cur_col_ = std::min(cols_ - 1, cur_col_ + std::max(1, param_at(p,0,1))); break;
        case 'D': cur_col_ = std::max(0, cur_col_ - std::max(1, param_at(p,0,1))); break;
        case 'G': cur_col_ = std::clamp(param_at(p,0,1) - 1, 0, cols_ - 1); break;
        case 'H': case 'f': {
            int row = param_at(p, 0, 1) - 1;
            int col = param_at(p, 1, 1) - 1;
            cur_row_ = std::clamp(row, 0, rows_ - 1);
            cur_col_ = std::clamp(col, 0, cols_ - 1);
            break;
        }
        case 'J': {
            int mode = param_at(p, 0, 0);
            if (mode == 2) {
                // Erase entire viewport (screen only; scrollback untouched).
                for (auto& r : screen_) std::fill(r.begin(), r.end(), Cell{});
            } else if (mode == 3) {
                // Erase saved lines — THE scrollback wipe. Drop all history.
                scrollback_.clear();
            }
            break;
        }
        case 'K': {
            int mode = param_at(p, 0, 0);
            auto& r = screen_[cur_row_];
            if (mode == 0) for (int x = cur_col_; x < cols_; ++x) r[x] = Cell{};
            else if (mode == 1) for (int x = 0; x <= cur_col_ && x < cols_; ++x) r[x] = Cell{};
            else for (int x = 0; x < cols_; ++x) r[x] = Cell{};
            break;
        }
        case 'm': break;        // SGR — irrelevant to geometry
        case 'h': case 'l': break;  // mode set/reset (sync output etc.)
        default: break;
        }
    }

    static void append_utf8(std::string& out, char32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
};

} // namespace vt

// ═════════════════════════════════════════════════════════════════════════
//  Host transcript model — what agentty/agent_session hands Strata.
// ═════════════════════════════════════════════════════════════════════════
//
// A node is a logical turn: a stable key, a content payload (lines), and a
// `terminal` flag (false while streaming, true once done). The host hands
// Strata the FULL ordered node list every frame plus a lazy builder.
struct HostNode {
    std::uint64_t key = 0;
    std::vector<std::string> lines;   // each entry is one rendered row of text
    bool terminal = false;
    std::uint64_t hash = 0;           // recomputed when lines/terminal change
};

static std::uint64_t fold_hash(const HostNode& n) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    mix(n.key);
    mix(n.terminal ? 0xABCDEF : 0x123456);
    for (const auto& ln : n.lines) {
        for (char c : ln) mix(static_cast<unsigned char>(c));
        mix(0x0A);
    }
    return h;
}

struct Host {
    std::vector<HostNode> nodes;

    std::vector<NodeRef> refs() const {
        std::vector<NodeRef> r;
        r.reserve(nodes.size());
        for (const auto& n : nodes) r.push_back({n.key, n.hash, n.terminal});
        return r;
    }

    const HostNode* find(std::uint64_t key) const {
        for (const auto& n : nodes)
            if (n.key == key) return &n;
        return nullptr;
    }
};

// Build a Column BoxElement of TextElement rows for one node.
static Element build_node(const HostNode& n) {
    BoxElement box;
    box.layout.direction = FlexDirection::Column;
    if (n.lines.empty()) {
        box.children.push_back(Element{TextElement{.content = ""}});
    } else {
        for (const auto& ln : n.lines)
            box.children.push_back(Element{TextElement{.content = ln}});
    }
    return Element{std::move(box)};
}

// ═════════════════════════════════════════════════════════════════════════
//  Driver — runs one Strata frame, capturing emitted bytes into a vt::Term.
// ═════════════════════════════════════════════════════════════════════════
class Driver {
public:
    Driver(int cols, int rows) : cols_(cols), rows_(rows), term_(cols, rows) {
        int fds[2];
        int rc = pipe(fds);
        CHECK(rc == 0);
        rfd_ = fds[0];
        int wflags = fcntl(fds[1], F_GETFL, 0);
        fcntl(fds[1], F_SETFL, wflags | O_NONBLOCK);
        int rflags = fcntl(fds[0], F_GETFL, 0);
        fcntl(fds[0], F_SETFL, rflags | O_NONBLOCK);
        writer_ = new Writer{static_cast<platform::NativeHandle>(fds[1])};
        wfd_ = fds[1];
    }
    ~Driver() {
        delete writer_;
        if (rfd_ >= 0) close(rfd_);
    }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    void set_geometry(int cols, int rows) {
        cols_ = cols; rows_ = rows;
        term_.resize(cols, rows);
    }

    // Run one frame against the host, then drain emitted bytes into the model.
    FrameStats frame(const Host& host) {
        auto refs = host.refs();
        FrameStats st = strata_.frame(
            refs,
            [&](std::uint64_t key) -> Element {
                const HostNode* n = host.find(key);
                CHECK(n != nullptr);
                return build_node(*n);
            },
            cols_, term_rows_for_test(rows_), pool_, *writer_);
        last_bytes_.clear();
        drain();
        return st;
    }

    const std::string& last_bytes() const { return last_bytes_; }

    void reset()      { strata_.reset(); }
    void reset_hard() { strata_.reset_hard(); }

    vt::Term&        term()  { return term_; }
    Strata&          strata(){ return strata_; }
    int cols() const { return cols_; }
    int rows() const { return rows_; }

private:
    void drain() {
        // Drain BOTH the Writer's buffered ops (flush) and the pipe.
        (void)writer_->flush();
        char buf[1 << 16];
        std::string acc;
        for (;;) {
            ssize_t n = read(rfd_, buf, sizeof(buf));
            if (n > 0) { acc.append(buf, static_cast<size_t>(n)); continue; }
            break;  // EAGAIN / 0 — nothing more right now
        }
        if (!acc.empty()) { last_bytes_ = acc; term_.feed(acc); }
    }

    std::string last_bytes_;

    int cols_, rows_;
    int rfd_ = -1, wfd_ = -1;
    StylePool pool_;
    Writer* writer_ = nullptr;
    Strata strata_;
    vt::Term term_;
};

// ═════════════════════════════════════════════════════════════════════════
//  The deposition invariant.
// ═════════════════════════════════════════════════════════════════════════
//
// After any frame in a NON-SWAP, NON-RESIZE steady run, every content row
// the host has produced for SEALED nodes must appear exactly once in the
// model's continuous buffer, in order, and live nodes must appear in the
// viewport. Rather than track per-row provenance (brittle across wrapping),
// we assert the strongest practical structural property:
//
//   (I1) No DUPLICATED non-blank content line anywhere in (scrollback ∪
//        screen) that came from a sealed node — i.e. the catastrophe
//        (re-emit a scrolled-off turn) never happens. We detect it by
//        tagging every host line with a globally-unique sentinel token
//        and asserting each token appears at most once in the buffer.
//
//   (I2) No LOST content: every token from a node that has fully scrolled
//        past the fold OR is on screen appears at least once.
//
// Tokens make this exact and wrapping-robust: each line begins with a
// unique "§<id>·" marker; we count marker occurrences across all rows.
struct Invariant {
    // marker id -> occurrences in the continuous buffer
    static void check(Driver& d, const Host& host, const char* phase) {
        check(d, host, phase, /*term_rows=*/0);
    }
    static void check(Driver& d, const Host& host, const char* phase,
                      int term_rows) {
        vt::Term& t = d.term();
        std::unordered_map<std::uint64_t, int> seen;
        for (int y = 0; y < t.total_rows(); ++y) {
            std::string row = t.row_text(y);
            // A row may contain at most one marker at its start (we never
            // wrap a marker — markers are short and rows are >= marker len).
            std::uint64_t id;
            if (parse_marker(row, id)) seen[id]++;
        }
        // Build the set of markers the host has emitted.
        for (const auto& n : host.nodes) {
            for (const auto& ln : n.lines) {
                std::uint64_t id;
                if (!parse_marker(ln, id)) continue;
                int c = seen.count(id) ? seen[id] : 0;
                // I1: never duplicated.
                if (c > 1) {
                    std::fprintf(stderr,
                        "\n  INVARIANT I1 (no-dup) FAILED in phase '%s': "
                        "marker %llu appears %dx\n", phase,
                        (unsigned long long)id, c);
                    dump(t, id);
                    CHECK(false);
                }
            }
        }

        // ── I2: LIVE TAIL VISIBILITY (no rows hidden while live) ─────────
        // I1 (no-dup) cannot catch a row that is temporarily HIDDEN — a
        // hidden marker has count 0, which I1 permits. But the whole point
        // of the live tail is that it is ON SCREEN: the host's newest,
        // not-yet-sealed content must be visible. The reported bug was
        // exactly this — a live node that overflowed the fold and then
        // re-wrapped shorter left committed_rows_ over-counted, so Section 3
        // windowed out rows that were still on screen; they vanished until
        // the turn sealed.
        //
        // Assert: the BOTTOM term_rows worth of band content (the live
        // suffix's most recent rows — the part that is unambiguously on
        // screen, never legitimately scrolled off) all appear in the
        // buffer. We walk the live (non-terminal) nodes' lines from the
        // newest backward and require the last `term_rows` of them present.
        // (Skipped for term_rows==0 callers and for swap/resize phases,
        // which intentionally repaint and may transiently differ.)
        if (term_rows > 0) {
            std::vector<std::uint64_t> live_tail;  // newest-first
            for (auto it = host.nodes.rbegin();
                 it != host.nodes.rend() && static_cast<int>(live_tail.size())
                     < term_rows; ++it) {
                if (it->terminal) break;   // sealed prefix is scrollback-owned
                for (auto lit = it->lines.rbegin();
                     lit != it->lines.rend()
                         && static_cast<int>(live_tail.size()) < term_rows; ++lit) {
                    std::uint64_t id;
                    if (parse_marker(*lit, id)) live_tail.push_back(id);
                }
            }
            for (std::uint64_t id : live_tail) {
                int c = seen.count(id) ? seen[id] : 0;
                if (c < 1) {
                    std::fprintf(stderr,
                        "\n  INVARIANT I2 (live-visible) FAILED in phase '%s': "
                        "live-tail marker %llu is HIDDEN (count=0) — a row "
                        "still on screen was windowed out.\n", phase,
                        (unsigned long long)id);
                    dump(t, id);
                    CHECK(false);
                }
            }
        }
        (void)phase;
    }

    static std::string marker(std::uint64_t id) {
        // "\u00A7" (§) + id + "\u00B7" (·) — both > 1 col? No: § and · are
        // width-1. Keep ASCII-safe so the model's row_text round-trips.
        return "§" + std::to_string(id) + "·";
    }
    // Print every row index whose text carries the duplicated marker, plus
    // a window of context, so we can see whether the dup is in scrollback,
    // on screen, or both.
    static void dump(vt::Term& t, std::uint64_t dup_id) {
        std::fprintf(stderr, "  buffer: total_rows=%d scroll_base=%d\n",
                     t.total_rows(), t.scroll_base());
        for (int y = 0; y < t.total_rows(); ++y) {
            std::uint64_t id;
            std::string row = t.row_text(y);
            bool hit = parse_marker(row, id) && id == dup_id;
            if (hit) {
                const char* zone = (y < t.scroll_base()) ? "SCROLLBACK" : "SCREEN";
                std::fprintf(stderr, "    >> row %3d [%s]: %.40s\n", y, zone,
                             row.c_str());
            }
        }
    }

    static bool parse_marker(const std::string& row, std::uint64_t& out) {
        // marker is "§<digits>·..."; § is 2 UTF-8 bytes (0xC2 0xA7).
        size_t i = 0;
        if (row.size() < 3) return false;
        if (static_cast<unsigned char>(row[0]) != 0xC2 ||
            static_cast<unsigned char>(row[1]) != 0xA7) return false;
        i = 2;
        std::uint64_t v = 0; bool any = false;
        while (i < row.size() && row[i] >= '0' && row[i] <= '9') {
            v = v * 10 + (row[i] - '0'); any = true; ++i;
        }
        if (!any) return false;
        out = v;
        return true;
    }
};

// ═════════════════════════════════════════════════════════════════════════
//  Operation log + fuzzer.
// ═════════════════════════════════════════════════════════════════════════
enum class Op : std::uint8_t {
    AppendNode,    // start a new streaming node
    GrowNode,      // add a line to the last (live) node
    ShrinkNode,    // DROP a line from the last (live) node — re-wrap/fold
    SealNode,      // mark the last node terminal
    ResizeWidth,   // change terminal width
    ResizeHeight,  // change terminal height
    SwapThread,    // wholesale surface swap (new transcript)
    Rewind,        // truncate the transcript below the frontier
    WideBurst,     // grow node with a wide-char (CJK/emoji) line
};

struct Action {
    Op op;
    int arg = 0;   // width/height for resizes, line count for bursts
};

static const char* op_name(Op o);

struct FuzzConfig {
    int    frames      = 4000;
    int    base_cols   = 80;
    int    base_rows   = 24;
    bool   verbose     = false;
};

// Run a fixed action log to completion, checking the invariant each frame.
// Returns true if it survived; false on the FIRST invariant break (the
// CHECK aborts, so a false return is only reachable in shrink mode where we
// trap via a flag instead — see run_log_trapping).
static void run_log(const std::vector<Action>& log, const FuzzConfig& cfg) {
    Driver d(cfg.base_cols, cfg.base_rows);
    Host host;
    std::uint64_t next_key = 1;
    std::uint64_t next_marker = 1;
    int cur_cols = cfg.base_cols, cur_rows = cfg.base_rows;
    int frame_no = 0;

    auto new_line = [&](bool wide) {
        std::string m = Invariant::marker(next_marker++);
        if (wide) m += "日本語テキスト絵文字😀😀";  // wide + emoji
        else      m += "the quick brown fox jumps";
        return m;
    };

    // A single turn's RENDERED height (lines; our text never wraps at the
    // floored widths >= 40 — each line is < 40 cols). The windowed compose
    // deposits a turn's overflow at row granularity, so turns taller than the
    // viewport are exercised here directly (test_giant_node pins one too).
    auto node_rows = [&](const HostNode& n) { return static_cast<int>(n.lines.size()); };
    // Windowed compose (committed_rows_) deposits OVERFLOW at row granularity,
    // so a single turn taller than the viewport is now in scope. Cap turns
    // generously above the viewport to exercise the straddling-node path
    // (a node whose committed head is windowed out while its live tail paints).
    const int max_turn_rows = std::max(2, std::min(cfg.base_rows, cur_rows) * 2);

    auto seal_tail = [&]() {
        // A real transcript finishes the previous turn before a new one
        // begins: the prior tail node is terminal once a fresh node arrives.
        if (!host.nodes.empty() && !host.nodes.back().terminal) {
            host.nodes.back().terminal = true;
            host.nodes.back().hash = fold_hash(host.nodes.back());
        }
    };

    for (const Action& a : log) {
        switch (a.op) {
        case Op::AppendNode: {
            seal_tail();
            HostNode n;
            n.key = next_key++;
            n.lines.push_back(new_line(false));
            n.terminal = false;
            n.hash = fold_hash(n);
            host.nodes.push_back(std::move(n));
            break;
        }
        case Op::GrowNode: {
            if (host.nodes.empty() || host.nodes.back().terminal
                || node_rows(host.nodes.back()) >= max_turn_rows) {
                seal_tail();
                HostNode n; n.key = next_key++;
                n.lines.push_back(new_line(false));
                n.hash = fold_hash(n);
                host.nodes.push_back(std::move(n));
            } else {
                host.nodes.back().lines.push_back(new_line(false));
                host.nodes.back().hash = fold_hash(host.nodes.back());
            }
            break;
        }
        case Op::WideBurst: {
            if (host.nodes.empty() || host.nodes.back().terminal
                || node_rows(host.nodes.back()) >= max_turn_rows) {
                seal_tail();
                HostNode n; n.key = next_key++;
                n.lines.push_back(new_line(true));
                n.hash = fold_hash(n);
                host.nodes.push_back(std::move(n));
            } else {
                int k = std::min<int>(std::max(1, a.arg),
                                      max_turn_rows - node_rows(host.nodes.back()));
                for (int i = 0; i < k; ++i)
                    host.nodes.back().lines.push_back(new_line(true));
                host.nodes.back().hash = fold_hash(host.nodes.back());
            }
            break;
        }
        case Op::ShrinkNode: {
            // Re-wrap / fold: the LIVE tail node's measured height drops
            // between frames (a wrapped line collapses shorter, a fenced
            // code block folds, the reveal clip retracts). Only a live
            // (non-terminal) node can reflow; a sealed node is immutable.
            // Keep at least one line. The dropped markers leave the host's
            // emitted set, so they are no longer required to be present.
            // This is the exact regression the live-shrink overflow
            // recovery (committed_rows_ clamp + case-B repaint) fixes:
            // before it, a node that overflowed the fold and then shrank
            // left committed_rows_ over-counted and Section 3 windowed out
            // rows still on screen.
            if (!host.nodes.empty() && !host.nodes.back().terminal
                && node_rows(host.nodes.back()) > 1) {
                int drop = std::clamp(a.arg, 1,
                    node_rows(host.nodes.back()) - 1);
                auto& ln = host.nodes.back().lines;
                ln.erase(ln.end() - drop, ln.end());
                host.nodes.back().hash = fold_hash(host.nodes.back());
            }
            break;
        }
        case Op::SealNode: {
            seal_tail();
            break;
        }
        case Op::ResizeWidth: {
            cur_cols = std::clamp(a.arg, 40, 200);
            d.set_geometry(cur_cols, cur_rows);
            break;
        }
        case Op::ResizeHeight: {
            cur_rows = std::clamp(a.arg, 6, 60);
            d.set_geometry(cur_cols, cur_rows);
            break;
        }
        case Op::SwapThread: {
            // Wholesale swap: brand-new transcript. Strata auto-detects via
            // the frontier fingerprint and arms a hard reset itself. The
            // model's scrollback is wiped by the \x1b[3J that follows, so
            // post-swap markers are all-new and can't collide with old ones.
            seal_tail();
            host.nodes.clear();
            HostNode n; n.key = next_key++;
            n.lines.push_back(new_line(false));
            n.terminal = false;
            n.hash = fold_hash(n);
            host.nodes.push_back(std::move(n));
            break;
        }
        case Op::Rewind: {
            // Truncate the transcript (transcript rewind / branch). Strata
            // sees nodes.size() < sealed_count_ or a frontier mismatch and
            // hard-resets. Keep at least one node.
            int keep = std::clamp(a.arg, 1,
                                  std::max(1, static_cast<int>(host.nodes.size())));
            if (static_cast<int>(host.nodes.size()) > keep)
                host.nodes.resize(keep);
            break;
        }
        }

        FrameStats st = d.frame(host);
        (void)st;
        if (cfg.verbose) {
            std::fprintf(stderr, "  frame %3d op=%-12s cols=%d rows=%d "
                         "nodes=%zu sealed=%d live=%d build=%d rep=%d coh=%d\n",
                         frame_no, op_name(a.op), cur_cols, cur_rows,
                         host.nodes.size(), st.sealed_now, st.live_nodes,
                         st.builds, st.full_repaint ? 1 : 0, st.coherence);
            if (frame_no >= cfg.frames - 4) {
                std::string esc;
                for (char c : d.last_bytes()) {
                    unsigned char u = (unsigned char)c;
                    if (u == 0x1b) esc += "\\e";
                    else if (u == '\n') esc += "\\n";
                    else if (u == '\r') esc += "\\r";
                    else if (u < 0x20) { char b[8]; std::snprintf(b,8,"\\x%02x",u); esc += b; }
                    else esc += c;
                }
                std::fprintf(stderr, "      bytes(%zu): %.300s\n",
                             d.last_bytes().size(), esc.c_str());
            }
        }
        // I2 (live-tail visibility) only applies in steady streaming
        // phases. Resize/swap/rewind intentionally repaint and the model
        // may transiently differ from the host's live set the same frame;
        // pass term_rows=0 there to run I1 (no-dup) alone.
        const bool steady =
            a.op == Op::AppendNode || a.op == Op::GrowNode
            || a.op == Op::ShrinkNode || a.op == Op::WideBurst
            || a.op == Op::SealNode;
        Invariant::check(d, host, op_name(a.op), steady ? cur_rows : 0);
        ++frame_no;
    }
}

static const char* op_name(Op o) {
    switch (o) {
    case Op::AppendNode:  return "AppendNode";
    case Op::GrowNode:    return "GrowNode";
    case Op::ShrinkNode:  return "ShrinkNode";
    case Op::SealNode:    return "SealNode";
    case Op::ResizeWidth: return "ResizeWidth";
    case Op::ResizeHeight:return "ResizeHeight";
    case Op::SwapThread:  return "SwapThread";
    case Op::Rewind:      return "Rewind";
    case Op::WideBurst:   return "WideBurst";
    }
    return "?";
}

// Generate a hostile, interleaved action log from a seed.
//
// SCOPE OF THE GUARANTEE PROVEN HERE. Strata composes a WINDOWED active
// layer (docs/internals/strata-renderer.md): it deposits overflow at ROW
// granularity via the committed_rows_ watermark, so the proof now covers the
// full hostile regime — streaming growth, turn sealing, new turns, wide/emoji
// content, height GROWTH *and drastic SHRINK*, a single turn taller than the
// viewport (the straddling-node path), modest rewinds, and wholesale thread
// swaps, all interleaved. The earlier whole-node depositional model excluded
// a viewport shrink that stranded more than a viewport of live content above
// the fold; the windowed compose makes that case correct by construction
// (the renderer never sees a row the terminal already owns), so it is now
// fuzzed directly here.
static std::vector<Action> gen_log(std::uint64_t seed, const FuzzConfig& cfg) {
    std::mt19937_64 rng(seed);
    std::vector<Action> log;
    log.reserve(cfg.frames);
    auto roll = [&](int lo, int hi) {
        return static_cast<int>(std::uniform_int_distribution<int>(lo, hi)(rng));
    };
    for (int i = 0; i < cfg.frames; ++i) {
        int r = roll(0, 99);
        Action a{};
        // Seal-heavy mix so the LIVE band (un-sealed turns) stays ≈ a
        // viewport: a real session seals each turn as the next begins, so
        // only the newest turn or two is ever live. This is the regime the
        // whole-node depositional seal is designed for.
        if      (r < 22) a.op = Op::GrowNode;
        else if (r < 32) { a.op = Op::WideBurst; a.arg = roll(1, 8); }  // multi-row burst
        else if (r < 40) { a.op = Op::ShrinkNode; a.arg = roll(1, 6); } // live re-wrap/fold
        else if (r < 60) { a.op = Op::SealNode; }     // seal often
        else if (r < 78) a.op = Op::AppendNode;        // append seals the prior tail
        else if (r < 90) { a.op = Op::ResizeHeight; a.arg = roll(6, 60); }   // incl. drastic shrink
        else if (r < 96) { a.op = Op::ResizeWidth;  a.arg = roll(40, 200); }
        else if (r < 98) { a.op = Op::SwapThread; }
        else             { a.op = Op::Rewind; a.arg = roll(1, 6); }
        log.push_back(a);
    }
    return log;
}

// ═════════════════════════════════════════════════════════════════════════
//  Entry point.
// ═════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    FuzzConfig cfg;
    int seeds = 24;
    if (argc > 1) seeds = std::atoi(argv[1]);
    if (argc > 2) cfg.frames = std::atoi(argv[2]);
    if (argc > 3) cfg.verbose = std::atoi(argv[3]) != 0;

    std::println("test_strata: adversarial deposition fuzz — {} seeds × {} frames",
                 seeds, cfg.frames);

    for (int s = 0; s < seeds; ++s) {
        std::uint64_t seed = 0x9e3779b97f4a7c15ULL * (s + 1);
        auto log = gen_log(seed, cfg);
        run_log(log, cfg);
        if (cfg.verbose || (s % 8 == 0))
            std::println("  seed {:>2} (0x{:x}): OK", s, seed);
    }

    std::println("test_strata: ALL PASS — deposition invariant held under fuzz");
    return 0;
}
