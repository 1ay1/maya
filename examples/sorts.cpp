// examples/sorts.cpp — eight sorting algorithms racing, as a jaal program.
//
// Bubble, selection, insertion, shell, quick, merge, heap and LSD radix sort
// run side by side on the same data, animated operation by operation, with
// compares, swaps and writes highlighted and a rainbow celebration when one
// finishes.
//
//   Model      the eight races: each is its array, its recorded operation
//              list (every compare / swap / write / "this is sorted" the
//              algorithm makes, in order), the replay position, counters and
//              highlights. Plus speed, input pattern, solo view, pause, the
//              elapsed time and the RNG.
//   update()   Tick replays `speed` operations of every race; keys restart,
//              change pattern or speed, pause, or solo one algorithm.
//   view()     a grid of panels (title, counters, progress, the bars as a
//              pixels() image) and a two-line status bar.
//
// Keys: space restart   p pattern   ←/→ ↑/↓ +/- speed   k pause
//       1-8 solo (again to unsolo)   0 show all   q quit

#include <maya/element/pixels.hpp>
#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int kAlgos = 8, kSize = 80, kCelebrate = 50, kTickMs = 33;
constexpr std::array<const char*, kAlgos> kNames = {"Bubble Sort", "Selection Sort", "Insertion Sort", "Shell Sort",
                                                    "Quick Sort",  "Merge Sort",     "Heap Sort",      "Radix Sort (LSD)"};
constexpr std::array<const char*, 5> kPatterns = {"Random", "Reversed", "Nearly Sorted", "Few Unique", "Pipe Organ"};

// ── a race: an algorithm recorded as operations, replayed a few per tick ─────

enum class Mark : std::uint8_t { None, Compare, Swap, Sorted, Active };
struct Op { enum Kind : std::uint8_t { Compare, Swap, Sorted, Set } kind; int i, j; };

struct Race {
    std::vector<int>  arr;
    std::vector<Op>   ops;
    std::vector<Mark> mark;
    std::size_t at = 0;
    int compares = 0, swaps = 0, celebrate = 0;
    bool done = false;

    [[nodiscard]] float progress() const { return done ? 1.f : ops.empty() ? 0.f : static_cast<float>(at) / ops.size(); }

    void step() {
        for (auto& k : mark) if (k != Mark::Sorted) k = Mark::None;
        if (at >= ops.size()) {
            if (!done) { done = true; std::ranges::fill(mark, Mark::Sorted); }
            return;
        }
        const Op op = ops[at++];
        const int n = static_cast<int>(arr.size());
        auto in = [n](int i) { return i >= 0 && i < n; };
        switch (op.kind) {
            case Op::Compare: ++compares; if (in(op.i)) mark[static_cast<std::size_t>(op.i)] = Mark::Compare;
                                          if (in(op.j)) mark[static_cast<std::size_t>(op.j)] = Mark::Compare; break;
            case Op::Swap: ++swaps;
                if (in(op.i) && in(op.j)) {
                    std::swap(arr[static_cast<std::size_t>(op.i)], arr[static_cast<std::size_t>(op.j)]);
                    mark[static_cast<std::size_t>(op.i)] = mark[static_cast<std::size_t>(op.j)] = Mark::Swap;
                }
                break;
            case Op::Set:    if (in(op.i)) { arr[static_cast<std::size_t>(op.i)] = op.j; mark[static_cast<std::size_t>(op.i)] = Mark::Active; } break;
            case Op::Sorted: if (in(op.i)) mark[static_cast<std::size_t>(op.i)] = Mark::Sorted; break;
        }
    }
};

// ── the algorithms, each run once on a copy to record its operations ─────────

using Ops = std::vector<Op>;

void bubble(std::vector<int> a, Ops& o) {
    const int n = static_cast<int>(a.size());
    for (int i = 0; i < n - 1; ++i) {
        bool swapped = false;
        for (int j = 0; j < n - 1 - i; ++j) {
            o.push_back({Op::Compare, j, j + 1});
            if (a[j] > a[j + 1]) { std::swap(a[j], a[j + 1]); o.push_back({Op::Swap, j, j + 1}); swapped = true; }
        }
        o.push_back({Op::Sorted, n - 1 - i, 0});
        if (!swapped) { for (int k = 0; k <= n - 2 - i; ++k) o.push_back({Op::Sorted, k, 0}); return; }
    }
    o.push_back({Op::Sorted, 0, 0});
}
void selection(std::vector<int> a, Ops& o) {
    const int n = static_cast<int>(a.size());
    for (int i = 0; i < n - 1; ++i) {
        int m = i;
        for (int j = i + 1; j < n; ++j) { o.push_back({Op::Compare, m, j}); if (a[j] < a[m]) m = j; }
        if (m != i) { std::swap(a[i], a[m]); o.push_back({Op::Swap, i, m}); }
        o.push_back({Op::Sorted, i, 0});
    }
    o.push_back({Op::Sorted, n - 1, 0});
}
void insertion(std::vector<int> a, Ops& o) {
    const int n = static_cast<int>(a.size());
    o.push_back({Op::Sorted, 0, 0});
    for (int i = 1; i < n; ++i) {
        for (int j = i; j > 0; --j) {
            o.push_back({Op::Compare, j - 1, j});
            if (a[j - 1] <= a[j]) break;
            std::swap(a[j - 1], a[j]); o.push_back({Op::Swap, j - 1, j});
        }
        o.push_back({Op::Sorted, i, 0});
    }
}
void shell(std::vector<int> a, Ops& o) {
    const int n = static_cast<int>(a.size());
    for (int gap : {301, 132, 57, 23, 10, 4, 1}) {
        if (gap >= n) continue;
        for (int i = gap; i < n; ++i)
            for (int j = i; j >= gap; j -= gap) {
                o.push_back({Op::Compare, j - gap, j});
                if (a[j - gap] <= a[j]) break;
                std::swap(a[j - gap], a[j]); o.push_back({Op::Swap, j - gap, j});
            }
    }
    for (int i = 0; i < n; ++i) o.push_back({Op::Sorted, i, 0});
}
void quick(std::vector<int> a, Ops& o) {            // median-of-three, explicit stack
    const int n = static_cast<int>(a.size());
    std::vector<std::pair<int, int>> stack{{0, n - 1}};
    auto order = [&](int x, int y) { o.push_back({Op::Compare, x, y}); if (a[x] > a[y]) { std::swap(a[x], a[y]); o.push_back({Op::Swap, x, y}); } };
    while (!stack.empty()) {
        auto [lo, hi] = stack.back(); stack.pop_back();
        if (lo >= hi) { if (lo >= 0 && lo < n) o.push_back({Op::Sorted, lo, 0}); continue; }
        const int mid = lo + (hi - lo) / 2;
        order(lo, mid); order(lo, hi); order(mid, hi);
        const int pivot = a[hi];
        int i = lo;
        for (int j = lo; j < hi; ++j) {
            o.push_back({Op::Compare, j, hi});
            if (a[j] <= pivot) { if (i != j) { std::swap(a[i], a[j]); o.push_back({Op::Swap, i, j}); } ++i; }
        }
        if (i != hi) { std::swap(a[i], a[hi]); o.push_back({Op::Swap, i, hi}); }
        o.push_back({Op::Sorted, i, 0});
        if (i - 1 - lo > hi - i - 1) { stack.push_back({lo, i - 1}); stack.push_back({i + 1, hi}); }
        else                         { stack.push_back({i + 1, hi}); stack.push_back({lo, i - 1}); }
    }
}
void merge(std::vector<int> a, Ops& o) {            // bottom-up
    const int n = static_cast<int>(a.size());
    for (int w = 1; w < n; w *= 2)
        for (int lo = 0; lo < n; lo += 2 * w) {
            const int mid = std::min(lo + w, n), hi = std::min(lo + 2 * w, n);
            std::vector<int> tmp;
            int i = lo, j = mid;
            while (i < mid && j < hi) { o.push_back({Op::Compare, i, j}); tmp.push_back(a[i] <= a[j] ? a[i++] : a[j++]); }
            while (i < mid) tmp.push_back(a[i++]);
            while (j < hi)  tmp.push_back(a[j++]);
            for (int k = 0; k < static_cast<int>(tmp.size()); ++k)
                if (a[lo + k] != tmp[static_cast<std::size_t>(k)]) { a[lo + k] = tmp[static_cast<std::size_t>(k)]; o.push_back({Op::Set, lo + k, a[lo + k]}); }
        }
    for (int i = 0; i < n; ++i) o.push_back({Op::Sorted, i, 0});
}
void heap(std::vector<int> a, Ops& o) {
    const int n = static_cast<int>(a.size());
    auto sift = [&](int root, int end) {
        while (2 * root + 1 <= end) {
            const int child = 2 * root + 1;
            int sw = root;
            o.push_back({Op::Compare, sw, child}); if (a[sw] < a[child]) sw = child;
            if (child + 1 <= end) { o.push_back({Op::Compare, sw, child + 1}); if (a[sw] < a[child + 1]) sw = child + 1; }
            if (sw == root) return;
            std::swap(a[root], a[sw]); o.push_back({Op::Swap, root, sw});
            root = sw;
        }
    };
    for (int s = (n - 2) / 2; s >= 0; --s) sift(s, n - 1);
    for (int end = n - 1; end > 0; --end) {
        std::swap(a[0], a[end]); o.push_back({Op::Swap, 0, end}); o.push_back({Op::Sorted, end, 0});
        sift(0, end - 1);
    }
    o.push_back({Op::Sorted, 0, 0});
}
void radix(std::vector<int> a, Ops& o) {            // LSD, base 10
    const int n = static_cast<int>(a.size()), top = *std::ranges::max_element(a);
    for (int exp = 1; top / exp > 0; exp *= 10) {
        std::array<int, 10> count{};
        for (int i = 0; i < n; ++i) { o.push_back({Op::Compare, i, i}); ++count[static_cast<std::size_t>((a[i] / exp) % 10)]; }
        for (std::size_t d = 1; d < 10; ++d) count[d] += count[d - 1];
        std::vector<int> out(static_cast<std::size_t>(n));
        for (int i = n - 1; i >= 0; --i) out[static_cast<std::size_t>(--count[static_cast<std::size_t>((a[i] / exp) % 10)])] = a[i];
        for (int i = 0; i < n; ++i) if (a[i] != out[static_cast<std::size_t>(i)]) { a[i] = out[static_cast<std::size_t>(i)]; o.push_back({Op::Set, i, a[i]}); }
    }
    for (int i = 0; i < n; ++i) o.push_back({Op::Sorted, i, 0});
}

using Algo = void (*)(std::vector<int>, Ops&);
constexpr std::array<Algo, kAlgos> kAlgo = {bubble, selection, insertion, shell, quick, merge, heap, radix};

// ── model, messages ──────────────────────────────────────────────────────────

struct Model {
    std::array<Race, kAlgos> races;
    int speed = 4;                 // operations per tick
    int pattern = 0;
    int solo = -1;                 // -1: all eight
    bool paused = false;
    int ticks = 0;                 // elapsed, in ticks (the timer)
    std::mt19937 rng{42};
};

struct Tick {};
struct Restart {};
struct NextPattern {};
struct Speed { int add, mul; };    // speed = clamp(speed * mul + add)
struct Pause {};
struct Solo  { int i; };           // -1: show all
struct Quit  {};
using Msg = std::variant<Tick, Restart, NextPattern, Speed, Pause, Solo, Quit>;

std::vector<int> make_data(Model& m) {
    std::vector<int> a(kSize);
    std::iota(a.begin(), a.end(), 1);
    switch (m.pattern) {
        case 0: std::ranges::shuffle(a, m.rng); break;
        case 1: std::ranges::reverse(a); break;
        case 2: for (int i = 0; i < kSize / 10; ++i) std::swap(a[m.rng() % kSize], a[m.rng() % kSize]); break;
        case 3: for (int i = 0; i < kSize; ++i) a[static_cast<std::size_t>(i)] = (i * 6 / kSize) * (kSize / 6) + kSize / 12;
                std::ranges::shuffle(a, m.rng); break;
        case 4: for (int i = 0; i < kSize; ++i) a[static_cast<std::size_t>(i)] = i < kSize / 2 ? i * 2 + 1 : (kSize - 1 - i) * 2 + 2; break;
    }
    return a;
}

void start(Model& m) {
    const auto data = make_data(m);
    for (int i = 0; i < kAlgos; ++i) {
        Race& r = m.races[static_cast<std::size_t>(i)];
        r = Race{};
        r.arr = data;
        r.mark.assign(data.size(), Mark::None);
        kAlgo[static_cast<std::size_t>(i)](data, r.ops);
    }
    m.ticks = 0;
}

// ── view helpers ─────────────────────────────────────────────────────────────

Rgb hue_rgb(float hue, float lift) {                 // hue in degrees; lift 0..1 raises the floor
    const float h = std::fmod(hue, 360.f) / 60.f, x = 1.f - std::fabs(std::fmod(h, 2.f) - 1.f);
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = 1; g = x; } else if (h < 2) { r = x; g = 1; } else if (h < 3) { g = 1; b = x; }
    else if (h < 4) { g = x; b = 1; } else if (h < 5) { r = x; b = 1; } else { r = 1; b = x; }
    auto c = [lift](float v) { return static_cast<std::uint8_t>(lift * 20 + v * (255 - lift * 20) * (lift > 0 ? 0.92f : 1.f)); };
    return {c(r), c(g), c(b)};
}

Rgb bar_colour(const Race& r, int i) {
    const int val = r.arr[static_cast<std::size_t>(i)];
    if (r.done && r.celebrate > 0 && r.celebrate <= kCelebrate
        && std::sin(i * 0.2f - r.celebrate * 0.3f) > -0.3f)
        return hue_rgb(static_cast<float>((i * 5 + r.celebrate * 7) % 360), 0);
    if (!r.done || r.celebrate == 0) switch (r.mark[static_cast<std::size_t>(i)]) {
        case Mark::Compare: return {40, 200, 100};
        case Mark::Swap:    return {255, 50, 50};
        case Mark::Active:  return {80, 160, 255};
        case Mark::Sorted:  return {255, 200, 40};
        case Mark::None:    break;
    }
    return hue_rgb(static_cast<float>(val) / kSize * 300.f, 1);
}

// The bars, bottom-up, as an image the panel's size (two pixels per row).
Image bars(const Race& r, int w, int rows) {
    const int ph = rows * 2;
    Image img(w, ph, {18, 18, 28});
    const float bw = static_cast<float>(w) / kSize;
    for (int i = 0; i < kSize; ++i) {
        const int x0 = static_cast<int>(i * bw), x1 = std::max(x0 + 1, static_cast<int>((i + 1) * bw));
        const int bh = std::max(1, r.arr[static_cast<std::size_t>(i)] * ph / kSize);
        const Rgb c = bar_colour(r, i);
        for (int x = x0; x < std::min(x1, w); ++x)
            for (int y = ph - bh; y < ph; ++y) img(x, y) = c;
    }
    return img;
}

struct Sorts {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key>;

    static Cmd init(Model& m) { start(m); return {}; }

    static Cmd update(Model& m, Tick) {
        if (m.paused) return {};
        ++m.ticks;
        for (auto& r : m.races) {
            if (!r.done) for (int k = 0; k < m.speed; ++k) r.step();
            else if (r.celebrate <= kCelebrate) ++r.celebrate;
        }
        return {};
    }
    static Cmd update(Model& m, Restart)     { m.rng.seed(std::random_device{}()); start(m); return {}; }
    static Cmd update(Model& m, NextPattern) { m.pattern = (m.pattern + 1) % 5; m.rng.seed(std::random_device{}()); start(m); return {}; }
    static Cmd update(Model& m, Speed s)     { m.speed = std::clamp(m.speed * s.mul + s.add, 1, 50); return {}; }
    static Cmd update(Model& m, Pause)       { m.paused = !m.paused; return {}; }
    static Cmd update(Model& m, Solo s)      { m.solo = (s.i < 0 || m.solo == s.i) ? -1 : s.i; return {}; }
    static Cmd update(Model&, Quit)          { return Cmd::quit(0); }

    static Element panel(const Race& r, int i) {
        const Color bg = Color::rgb(18, 18, 28), dim = Color::rgb(90, 90, 110);
        const auto title = std::string(" ") + kNames[static_cast<std::size_t>(i)] + (r.done ? " ✔" : "");
        char counts[48]; std::snprintf(counts, sizeof counts, " cmp:%-5d swp:%-5d", r.compares, r.swaps);
        const int pct = static_cast<int>(r.progress() * 100);
        return v(text(title) | fgc(r.done ? Color::rgb(80, 255, 130) : Color::rgb(170, 200, 255)) | Bold,
                 text(counts) | fgc(dim),
                 h(component([p = r.progress()](int w, int) {
                       const int filled = static_cast<int>(p * w);
                       std::string s;
                       for (int x = 0; x < w; ++x) s += x < filled ? "█" : "░";
                       return Element{text(s) | fgc(Color::rgb(80, 180, 255))};
                   }).grow(1),
                   text(" " + std::to_string(pct) + "%") | fgc(dim)),
                 paint([r](Canvas& c, int x, int y, int w, int hh) {
                     detail::paint_pixels(c, bars(r, w, hh), x, y, w, hh);
                 }).grow(1)) | bgc(bg) | grow_<1>;
    }

    static Element grid(const Model& m) {
        if (m.solo >= 0) return panel(m.races[static_cast<std::size_t>(m.solo)], m.solo);
        auto row = [&](int from) {
            return h(panel(m.races[static_cast<std::size_t>(from)], from),     panel(m.races[static_cast<std::size_t>(from + 1)], from + 1),
                     panel(m.races[static_cast<std::size_t>(from + 2)], from + 2), panel(m.races[static_cast<std::size_t>(from + 3)], from + 3)) | grow_<1>;
        };
        return v(row(0), row(4)) | grow_<1>;
    }

    static Element status(const Model& m) {
        const Color bar = Color::rgb(22, 22, 35);
        char timer[16]; std::snprintf(timer, sizeof timer, "%d:%02d ", m.ticks * kTickMs / 60000, m.ticks * kTickMs / 1000 % 60);
        return v(h(text(std::string(" [space] restart  [p] pattern  [←→] speed  [k] ") + (m.paused ? "resume" : "pause") +
                        "  [1-8] solo  [q] quit") | fgc(Color::rgb(130, 130, 160)),
                   spacer(), text(" SORTING VISUALIZER ") | fgc(Color::rgb(255, 180, 60)) | Bold) | bgc(bar),
                 h(text(" Speed: " + std::to_string(m.speed) + "x") | fgc(Color::rgb(100, 220, 255)) | Bold,
                   text(std::string("  Pattern: ") + kPatterns[static_cast<std::size_t>(m.pattern)]) | fgc(Color::rgb(200, 200, 220)),
                   text(m.paused ? "  ⏸ PAUSED" : "") | fgc(Color::rgb(255, 180, 60)) | Bold,
                   spacer(), text(timer) | fgc(Color::rgb(90, 90, 110))) | bgc(bar));
    }

    static Element view(const Model& m) { return v(grid(m), status(m)) | bgc(Color::rgb(12, 12, 20)); }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(std::chrono::milliseconds(kTickMs), Tick{}),
            keys<Sub>({
                {' ', Restart{}}, {'p', NextPattern{}}, {'k', Pause{}},
                {'+', Speed{1, 1}}, {'=', Speed{1, 1}}, {'-', Speed{-1, 1}},
                {SpecialKey::Right, Speed{3, 1}}, {SpecialKey::Left, Speed{-3, 1}},
                {SpecialKey::Up, Speed{0, 2}}, {SpecialKey::Down, Speed{0, 1}},
                {'1', Solo{0}}, {'2', Solo{1}}, {'3', Solo{2}}, {'4', Solo{3}},
                {'5', Solo{4}}, {'6', Solo{5}}, {'7', Solo{6}}, {'8', Solo{7}}, {'0', Solo{-1}},
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Sorts>);

}  // namespace

int main() { return run<Sorts>({.title = "sorting visualizer"}); }
