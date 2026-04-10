// maya -- Sorting Algorithm Visualizer
//
// Eight sorting algorithms race side-by-side with half-block bars for
// double vertical resolution. Watch each algorithm's unique access
// pattern emerge: bubble's steady sweeps, quick's recursive partitions,
// shell's diminishing gaps, radix's digit buckets.
//
// Keys: q/Esc=quit  space=restart  p=pattern  +/-/←/→=speed
//       1-8=solo algorithm  0=show all

#include <maya/internal.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace maya;

// -- Constants ---------------------------------------------------------------

static constexpr int NUM_ALGOS = 8;
static constexpr int ARRAY_SIZE = 80;
static constexpr int CELEBRATION_FRAMES = 50;

// -- Sorting state -----------------------------------------------------------

enum class HighlightKind { None, Compare, Swap, Sorted, Active };

struct SortState {
    const char* name;
    std::vector<int> arr;
    int comparisons = 0;
    int swaps       = 0;
    bool done       = false;
    int celebration_frame = 0;

    std::vector<HighlightKind> highlight;

    struct Op {
        enum Type { COMPARE, SWAP, MARK_SORTED, SET } type;
        int i, j;
    };
    std::vector<Op> ops;
    size_t op_idx = 0;

    void reset(const std::vector<int>& base) {
        arr = base;
        comparisons = 0;
        swaps = 0;
        done = false;
        celebration_frame = 0;
        highlight.assign(arr.size(), HighlightKind::None);
        ops.clear();
        op_idx = 0;
    }

    void step() {
        for (auto& h : highlight)
            if (h != HighlightKind::Sorted) h = HighlightKind::None;

        if (op_idx >= ops.size()) {
            if (!done) {
                done = true;
                celebration_frame = 0;
                for (auto& h : highlight) h = HighlightKind::Sorted;
            }
            return;
        }

        auto& op = ops[op_idx++];
        int n = static_cast<int>(arr.size());
        switch (op.type) {
            case Op::COMPARE:
                ++comparisons;
                if (op.i >= 0 && op.i < n) highlight[op.i] = HighlightKind::Compare;
                if (op.j >= 0 && op.j < n) highlight[op.j] = HighlightKind::Compare;
                break;
            case Op::SWAP:
                ++swaps;
                if (op.i >= 0 && op.i < n && op.j >= 0 && op.j < n) {
                    std::swap(arr[op.i], arr[op.j]);
                    highlight[op.i] = HighlightKind::Swap;
                    highlight[op.j] = HighlightKind::Swap;
                }
                break;
            case Op::SET:
                // Direct assignment (used by merge sort, radix sort)
                if (op.i >= 0 && op.i < n) {
                    arr[op.i] = op.j;
                    highlight[op.i] = HighlightKind::Active;
                }
                break;
            case Op::MARK_SORTED:
                if (op.i >= 0 && op.i < n) highlight[op.i] = HighlightKind::Sorted;
                break;
        }
    }

    [[nodiscard]] float progress() const {
        if (done) return 1.f;
        if (ops.empty()) return 0.f;
        return static_cast<float>(op_idx) / static_cast<float>(ops.size());
    }
};

// -- Pre-generate operation lists for each algorithm -------------------------

static void gen_bubble_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());
    for (int i = 0; i < n - 1; ++i) {
        bool swapped = false;
        for (int j = 0; j < n - 1 - i; ++j) {
            s.ops.push_back({SortState::Op::COMPARE, j, j + 1});
            if (a[j] > a[j + 1]) {
                std::swap(a[j], a[j + 1]);
                s.ops.push_back({SortState::Op::SWAP, j, j + 1});
                swapped = true;
            }
        }
        s.ops.push_back({SortState::Op::MARK_SORTED, n - 1 - i, 0});
        if (!swapped) {
            // Already sorted — mark remaining
            for (int k = 0; k <= n - 2 - i; ++k)
                s.ops.push_back({SortState::Op::MARK_SORTED, k, 0});
            break;
        }
    }
    if (!s.ops.empty() && s.ops.back().type != SortState::Op::MARK_SORTED)
        s.ops.push_back({SortState::Op::MARK_SORTED, 0, 0});
}

static void gen_selection_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());
    for (int i = 0; i < n - 1; ++i) {
        int min_idx = i;
        for (int j = i + 1; j < n; ++j) {
            s.ops.push_back({SortState::Op::COMPARE, min_idx, j});
            if (a[j] < a[min_idx]) min_idx = j;
        }
        if (min_idx != i) {
            std::swap(a[i], a[min_idx]);
            s.ops.push_back({SortState::Op::SWAP, i, min_idx});
        }
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});
    }
    s.ops.push_back({SortState::Op::MARK_SORTED, n - 1, 0});
}

static void gen_insertion_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());
    s.ops.push_back({SortState::Op::MARK_SORTED, 0, 0});
    for (int i = 1; i < n; ++i) {
        int j = i;
        while (j > 0) {
            s.ops.push_back({SortState::Op::COMPARE, j - 1, j});
            if (a[j - 1] > a[j]) {
                std::swap(a[j - 1], a[j]);
                s.ops.push_back({SortState::Op::SWAP, j - 1, j});
                --j;
            } else {
                break;
            }
        }
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});
    }
}

static void gen_shell_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());

    // Ciura's gap sequence (extended)
    int gaps[] = {301, 132, 57, 23, 10, 4, 1};
    for (int gap : gaps) {
        if (gap >= n) continue;
        for (int i = gap; i < n; ++i) {
            int j = i;
            while (j >= gap) {
                s.ops.push_back({SortState::Op::COMPARE, j - gap, j});
                if (a[j - gap] > a[j]) {
                    std::swap(a[j - gap], a[j]);
                    s.ops.push_back({SortState::Op::SWAP, j - gap, j});
                    j -= gap;
                } else {
                    break;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i)
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});
}

static void gen_quicksort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());

    struct Frame { int lo, hi; };
    std::vector<Frame> stack;
    stack.push_back({0, n - 1});

    while (!stack.empty()) {
        auto [lo, hi] = stack.back();
        stack.pop_back();
        if (lo >= hi) {
            if (lo >= 0 && lo < n)
                s.ops.push_back({SortState::Op::MARK_SORTED, lo, 0});
            continue;
        }

        // Median-of-three pivot
        int mid = lo + (hi - lo) / 2;
        s.ops.push_back({SortState::Op::COMPARE, lo, mid});
        if (a[lo] > a[mid]) { std::swap(a[lo], a[mid]); s.ops.push_back({SortState::Op::SWAP, lo, mid}); }
        s.ops.push_back({SortState::Op::COMPARE, lo, hi});
        if (a[lo] > a[hi])  { std::swap(a[lo], a[hi]);  s.ops.push_back({SortState::Op::SWAP, lo, hi}); }
        s.ops.push_back({SortState::Op::COMPARE, mid, hi});
        if (a[mid] > a[hi]) { std::swap(a[mid], a[hi]); s.ops.push_back({SortState::Op::SWAP, mid, hi}); }

        int pivot = a[hi];
        int i = lo;
        for (int j = lo; j < hi; ++j) {
            s.ops.push_back({SortState::Op::COMPARE, j, hi});
            if (a[j] <= pivot) {
                if (i != j) {
                    std::swap(a[i], a[j]);
                    s.ops.push_back({SortState::Op::SWAP, i, j});
                }
                ++i;
            }
        }
        if (i != hi) {
            std::swap(a[i], a[hi]);
            s.ops.push_back({SortState::Op::SWAP, i, hi});
        }
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});

        if (i - 1 - lo > hi - i - 1) {
            stack.push_back({lo, i - 1});
            stack.push_back({i + 1, hi});
        } else {
            stack.push_back({i + 1, hi});
            stack.push_back({lo, i - 1});
        }
    }
}

static void gen_merge_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());

    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid_val = std::min(lo + width, n);
            int hi  = std::min(lo + 2 * width, n);

            std::vector<int> tmp;
            int i = lo, j = mid_val;
            while (i < mid_val && j < hi) {
                s.ops.push_back({SortState::Op::COMPARE, i, j});
                if (a[i] <= a[j]) tmp.push_back(a[i++]);
                else              tmp.push_back(a[j++]);
            }
            while (i < mid_val) tmp.push_back(a[i++]);
            while (j < hi)      tmp.push_back(a[j++]);

            for (int k = 0; k < static_cast<int>(tmp.size()); ++k) {
                if (a[lo + k] != tmp[k]) {
                    a[lo + k] = tmp[k];
                    s.ops.push_back({SortState::Op::SET, lo + k, tmp[k]});
                }
            }
        }
    }
    for (int i = 0; i < n; ++i)
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});
}

static void gen_heap_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());

    auto sift_down = [&](int start, int end) {
        int root = start;
        while (2 * root + 1 <= end) {
            int child = 2 * root + 1;
            int sw = root;
            s.ops.push_back({SortState::Op::COMPARE, sw, child});
            if (a[sw] < a[child]) sw = child;
            if (child + 1 <= end) {
                s.ops.push_back({SortState::Op::COMPARE, sw, child + 1});
                if (a[sw] < a[child + 1]) sw = child + 1;
            }
            if (sw == root) break;
            std::swap(a[root], a[sw]);
            s.ops.push_back({SortState::Op::SWAP, root, sw});
            root = sw;
        }
    };

    for (int start = (n - 2) / 2; start >= 0; --start)
        sift_down(start, n - 1);

    for (int end = n - 1; end > 0; --end) {
        std::swap(a[0], a[end]);
        s.ops.push_back({SortState::Op::SWAP, 0, end});
        s.ops.push_back({SortState::Op::MARK_SORTED, end, 0});
        sift_down(0, end - 1);
    }
    s.ops.push_back({SortState::Op::MARK_SORTED, 0, 0});
}

static void gen_radix_sort(SortState& s) {
    auto a = s.arr;
    int n = static_cast<int>(a.size());
    int max_val = *std::max_element(a.begin(), a.end());

    for (int exp = 1; max_val / exp > 0; exp *= 10) {
        std::vector<int> output(n);
        int count[10] = {};

        for (int i = 0; i < n; ++i) {
            s.ops.push_back({SortState::Op::COMPARE, i, i}); // read access
            count[(a[i] / exp) % 10]++;
        }
        for (int i = 1; i < 10; ++i) count[i] += count[i - 1];

        for (int i = n - 1; i >= 0; --i) {
            int digit = (a[i] / exp) % 10;
            output[count[digit] - 1] = a[i];
            count[digit]--;
        }

        for (int i = 0; i < n; ++i) {
            if (a[i] != output[i]) {
                a[i] = output[i];
                s.ops.push_back({SortState::Op::SET, i, output[i]});
            }
        }
    }
    for (int i = 0; i < n; ++i)
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});
}

// -- Color helpers -----------------------------------------------------------

static Color value_color(int val, int max_val) {
    float t = (max_val > 0) ? static_cast<float>(val) / static_cast<float>(max_val) : 0.f;
    float hue = t * 300.f; // 0=red -> 60=yellow -> 120=green -> 180=cyan -> 240=blue -> 300=magenta
    float h = std::fmod(hue, 360.f) / 60.f;
    float c = 0.9f, x = c * (1.f - std::fabs(std::fmod(h, 2.f) - 1.f));
    float r = 0, g = 0, b = 0;
    if (h < 1)      { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    return Color::rgb(
        static_cast<uint8_t>(r * 235 + 20),
        static_cast<uint8_t>(g * 235 + 20),
        static_cast<uint8_t>(b * 235 + 20));
}

// -- Global state ------------------------------------------------------------

static std::mt19937 g_rng{42};
static SortState g_sorts[NUM_ALGOS];
static int g_solo = -1;       // -1 = show all, 0-7 = solo one
static int g_frame = 0;
static int g_ops_per_frame = 4;
static int g_pattern = 0;     // 0=random, 1=reversed, 2=nearly sorted, 3=few unique, 4=pipe organ
static constexpr int NUM_PATTERNS = 5;
static const char* g_pattern_names[] = {"Random", "Reversed", "Nearly Sorted", "Few Unique", "Pipe Organ"};
static bool g_paused = false;
static auto g_start_time = std::chrono::steady_clock::now();

// Style caches
static uint16_t S_BG, S_PANEL_BG;
static uint16_t S_DIM, S_TITLE, S_TITLE_DONE;
static uint16_t S_COMPARE, S_SWAP, S_SORTED, S_ACTIVE;
static uint16_t S_BORDER, S_PROGRESS_FG, S_PROGRESS_BG;
static uint16_t S_BAR_BG, S_BAR_FG, S_BAR_ACCENT;
static uint16_t S_SPEED;

static uint16_t S_VAL_BAR[ARRAY_SIZE];

static constexpr int CELEB_HUES = 72;
static uint16_t S_CELEB[CELEB_HUES];

static std::vector<int> gen_pattern(int size, int pattern) {
    std::vector<int> base(size);
    std::iota(base.begin(), base.end(), 1);

    switch (pattern) {
        case 0: // Random
            std::shuffle(base.begin(), base.end(), g_rng);
            break;
        case 1: // Reversed
            std::reverse(base.begin(), base.end());
            break;
        case 2: // Nearly sorted (90% sorted, 10% random swaps)
            for (int i = 0; i < size / 10; ++i) {
                int a = g_rng() % size, b = g_rng() % size;
                std::swap(base[a], base[b]);
            }
            break;
        case 3: // Few unique (only 6 distinct values)
            for (int i = 0; i < size; ++i)
                base[i] = (i * 6 / size) * (size / 6) + size / 12;
            std::shuffle(base.begin(), base.end(), g_rng);
            break;
        case 4: // Pipe organ (ascending then descending)
            for (int i = 0; i < size; ++i)
                base[i] = (i < size / 2) ? (i * 2 + 1) : ((size - 1 - i) * 2 + 2);
            break;
    }
    return base;
}

static void init_data() {
    auto base = gen_pattern(ARRAY_SIZE, g_pattern);

    static const char* names[] = {
        "Bubble Sort", "Selection Sort", "Insertion Sort", "Shell Sort",
        "Quick Sort", "Merge Sort", "Heap Sort", "Radix Sort (LSD)"
    };
    using GenFn = void(*)(SortState&);
    static const GenFn generators[] = {
        gen_bubble_sort, gen_selection_sort, gen_insertion_sort, gen_shell_sort,
        gen_quicksort, gen_merge_sort, gen_heap_sort, gen_radix_sort
    };

    for (int i = 0; i < NUM_ALGOS; ++i) {
        g_sorts[i].name = names[i];
        g_sorts[i].reset(base);
        generators[i](g_sorts[i]);
    }
    g_frame = 0;
    g_start_time = std::chrono::steady_clock::now();
}

// -- Render helpers ----------------------------------------------------------

static uint16_t style_for_bar(int val, HighlightKind hk, bool celebrating,
                              int celeb_frame, int bar_index) {
    if (celebrating && celeb_frame > 0 && celeb_frame <= CELEBRATION_FRAMES) {
        float wave = std::sin(static_cast<float>(bar_index) * 0.2f -
                              static_cast<float>(celeb_frame) * 0.3f);
        if (wave > -0.3f) {
            int hue_idx = ((bar_index * 5 + celeb_frame * 7) % 360);
            int ci = (hue_idx * CELEB_HUES) / 360;
            return S_CELEB[std::clamp(ci, 0, CELEB_HUES - 1)];
        }
    }

    if (!celebrating || celeb_frame == 0) {
        if (hk == HighlightKind::Compare) return S_COMPARE;
        if (hk == HighlightKind::Swap)    return S_SWAP;
        if (hk == HighlightKind::Active)  return S_ACTIVE;
        if (hk == HighlightKind::Sorted)  return S_SORTED;
    }

    int idx = std::clamp(val - 1, 0, ARRAY_SIZE - 1);
    return S_VAL_BAR[idx];
}

static void draw_panel(Canvas& canvas, int px, int py, int pw, int ph,
                       SortState& s, int max_val) {
    if (pw < 4 || ph < 6) return;

    // Panel background
    canvas.fill({{Columns{px}, Rows{py}}, {Columns{pw}, Rows{ph}}}, U' ', S_PANEL_BG);

    // Header: name
    char buf[128];
    if (s.done) {
        std::snprintf(buf, sizeof(buf), " %s ", s.name);
        canvas.write_text(px, py, buf, S_TITLE_DONE);
        // Show checkmark
        canvas.write_text(px + static_cast<int>(std::strlen(buf)), py, "\u2714", S_TITLE_DONE);
    } else {
        std::snprintf(buf, sizeof(buf), " %s", s.name);
        canvas.write_text(px, py, buf, S_TITLE);
    }

    // Stats line
    std::snprintf(buf, sizeof(buf), " cmp:%-5d swp:%-5d", s.comparisons, s.swaps);
    canvas.write_text(px, py + 1, buf, S_DIM);

    // Progress bar (row 2)
    float pct = s.progress();
    int bar_w = pw - 2;
    int filled = static_cast<int>(pct * static_cast<float>(bar_w));
    canvas.set(px, py + 2, U' ', S_PANEL_BG);
    for (int x = 0; x < bar_w; ++x) {
        uint16_t sid = (x < filled) ? S_PROGRESS_FG : S_PROGRESS_BG;
        canvas.set(px + 1 + x, py + 2, (x < filled) ? U'\u2588' : U'\u2591', sid);
    }
    // Percentage on right
    std::snprintf(buf, sizeof(buf), "%3d%%", static_cast<int>(pct * 100.f));
    int pct_x = px + pw - 5;
    if (pct_x > px + 1)
        canvas.write_text(pct_x, py + 2, buf, S_DIM);

    // Bar area (using half-block rendering for double vertical resolution)
    int bar_top = py + 3;
    int bar_height = (ph - 3) * 2; // double resolution
    if (bar_height <= 0) return;
    int rows = ph - 3;

    int n = static_cast<int>(s.arr.size());
    float bar_w_f = static_cast<float>(pw) / static_cast<float>(n);

    for (int i = 0; i < n; ++i) {
        int bx = px + static_cast<int>(static_cast<float>(i) * bar_w_f);
        int bw = std::max(1, static_cast<int>(static_cast<float>(i + 1) * bar_w_f) -
                             static_cast<int>(static_cast<float>(i) * bar_w_f));

        int val = s.arr[i];
        int bh = std::max(1, static_cast<int>(
            static_cast<float>(val) / static_cast<float>(max_val) * static_cast<float>(bar_height)));

        uint16_t sid = style_for_bar(val, s.highlight[i], s.done,
                                     s.celebration_frame, i);

        // Draw from bottom up using half-block characters
        // Each terminal row represents 2 sub-rows
        for (int row = 0; row < rows; ++row) {
            int y = bar_top + rows - 1 - row;
            int sub_lo = row * 2;       // bottom sub-pixel of this row
            int sub_hi = row * 2 + 1;   // top sub-pixel of this row
            bool lo_filled = sub_lo < bh;
            bool hi_filled = sub_hi < bh;

            for (int dx = 0; dx < bw; ++dx) {
                int x = bx + dx;
                if (x >= px + pw) break;
                if (lo_filled && hi_filled) {
                    canvas.set(x, y, U' ', sid);          // full block (bg color)
                } else if (lo_filled) {
                    canvas.set(x, y, U'\u2584', sid);     // lower half block ▄
                }
                // else: empty (panel bg already fills it)
            }
        }
    }
}

// -- Main --------------------------------------------------------------------

int main() {
    init_data();

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen,
                     .title = "sorting visualizer"},

        // on_resize
        [](StylePool& pool, int, int) {
            S_BG          = pool.intern(Style{}.with_bg(Color::rgb(12, 12, 20)));
            S_PANEL_BG    = pool.intern(Style{}.with_bg(Color::rgb(18, 18, 28)));
            S_DIM         = pool.intern(Style{}.with_bg(Color::rgb(18, 18, 28)).with_fg(Color::rgb(90, 90, 110)));
            S_TITLE       = pool.intern(Style{}.with_bg(Color::rgb(18, 18, 28)).with_fg(Color::rgb(170, 200, 255)).with_bold());
            S_TITLE_DONE  = pool.intern(Style{}.with_bg(Color::rgb(18, 18, 28)).with_fg(Color::rgb(80, 255, 130)).with_bold());
            S_COMPARE     = pool.intern(Style{}.with_bg(Color::rgb(40, 200, 100)));
            S_SWAP        = pool.intern(Style{}.with_bg(Color::rgb(255, 50, 50)));
            S_ACTIVE      = pool.intern(Style{}.with_bg(Color::rgb(80, 160, 255)));
            S_SORTED      = pool.intern(Style{}.with_bg(Color::rgb(255, 200, 40)));
            S_BORDER      = pool.intern(Style{}.with_fg(Color::rgb(40, 40, 60)).with_bg(Color::rgb(12, 12, 20)));
            S_PROGRESS_FG = pool.intern(Style{}.with_fg(Color::rgb(80, 180, 255)).with_bg(Color::rgb(18, 18, 28)));
            S_PROGRESS_BG = pool.intern(Style{}.with_fg(Color::rgb(35, 35, 50)).with_bg(Color::rgb(18, 18, 28)));
            S_BAR_BG      = pool.intern(Style{}.with_bg(Color::rgb(22, 22, 35)).with_fg(Color::rgb(130, 130, 160)));
            S_BAR_FG      = pool.intern(Style{}.with_bg(Color::rgb(22, 22, 35)).with_fg(Color::rgb(200, 200, 220)));
            S_BAR_ACCENT  = pool.intern(Style{}.with_bg(Color::rgb(22, 22, 35)).with_fg(Color::rgb(255, 180, 60)).with_bold());
            S_SPEED       = pool.intern(Style{}.with_bg(Color::rgb(22, 22, 35)).with_fg(Color::rgb(100, 220, 255)).with_bold());

            for (int i = 0; i < ARRAY_SIZE; ++i) {
                Color c = value_color(i + 1, ARRAY_SIZE);
                S_VAL_BAR[i] = pool.intern(Style{}.with_bg(c));
            }

            for (int i = 0; i < CELEB_HUES; ++i) {
                float hue = static_cast<float>(i) * 360.f / static_cast<float>(CELEB_HUES);
                float h = hue / 60.f;
                float cc = 1.f, xx = 1.f - std::fabs(std::fmod(h, 2.f) - 1.f);
                float r = 0, g = 0, b = 0;
                if (h < 1)      { r = cc; g = xx; }
                else if (h < 2) { r = xx; g = cc; }
                else if (h < 3) { g = cc; b = xx; }
                else if (h < 4) { g = xx; b = cc; }
                else if (h < 5) { r = xx; b = cc; }
                else             { r = cc; b = xx; }
                S_CELEB[i] = pool.intern(Style{}.with_bg(Color::rgb(
                    static_cast<uint8_t>(r * 255),
                    static_cast<uint8_t>(g * 255),
                    static_cast<uint8_t>(b * 255))));
            }
        },

        // on_event
        [](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

            // Restart
            on(ev, ' ', [] {
                g_rng.seed(static_cast<unsigned>(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
                init_data();
            });

            // Pattern selection
            on(ev, 'p', [] {
                g_pattern = (g_pattern + 1) % NUM_PATTERNS;
                g_rng.seed(static_cast<unsigned>(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
                init_data();
            });

            // Pause
            on(ev, 'k', [] { g_paused = !g_paused; });

            // Speed controls
            on(ev, '+', [] { g_ops_per_frame = std::min(50, g_ops_per_frame + 1); });
            on(ev, '=', [] { g_ops_per_frame = std::min(50, g_ops_per_frame + 1); });
            on(ev, '-', [] { g_ops_per_frame = std::max(1,  g_ops_per_frame - 1); });
            on(ev, SpecialKey::Right, [] { g_ops_per_frame = std::min(50, g_ops_per_frame + 3); });
            on(ev, SpecialKey::Left,  [] { g_ops_per_frame = std::max(1,  g_ops_per_frame - 3); });
            on(ev, SpecialKey::Up,    [] { g_ops_per_frame = std::min(50, g_ops_per_frame * 2); });
            on(ev, SpecialKey::Down,  [] { g_ops_per_frame = std::max(1,  g_ops_per_frame / 2); });

            // Solo toggles (1-8)
            for (int i = 0; i < NUM_ALGOS; ++i) {
                on(ev, static_cast<char>('1' + i), [i] {
                    g_solo = (g_solo == i) ? -1 : i;
                });
            }
            on(ev, '0', [] { g_solo = -1; });

            return true;
        },

        // on_paint
        [](Canvas& canvas, int W, int H) {
            ++g_frame;

            // Fill background
            canvas.fill({{Columns{0}, Rows{0}}, {Columns{W}, Rows{H}}}, U' ', S_BG);

            // Step sorting algorithms
            if (!g_paused) {
                for (int i = 0; i < NUM_ALGOS; ++i) {
                    auto& s = g_sorts[i];
                    if (!s.done) {
                        for (int k = 0; k < g_ops_per_frame; ++k)
                            s.step();
                    } else if (s.celebration_frame <= CELEBRATION_FRAMES) {
                        ++s.celebration_frame;
                    }
                }
            }

            // Timer
            auto elapsed = std::chrono::steady_clock::now() - g_start_time;
            int secs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());

            // Status bar (bottom 2 rows)
            int bar_y = H - 2;
            canvas.fill({{Columns{0}, Rows{bar_y}}, {Columns{W}, Rows{2}}}, U' ', S_BAR_BG);

            // Top status line: controls
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                " [space] restart  [p] pattern  [\u2190\u2192] speed  [k] %s  [1-%d] solo  [q] quit",
                g_paused ? "resume" : "pause", NUM_ALGOS);
            canvas.write_text(0, bar_y, buf, S_BAR_BG);

            // Bottom status line: speed + pattern + timer
            std::snprintf(buf, sizeof(buf), " Speed: %dx", g_ops_per_frame);
            canvas.write_text(0, bar_y + 1, buf, S_SPEED);

            int off = static_cast<int>(std::strlen(buf));
            std::snprintf(buf, sizeof(buf), "  Pattern: %s", g_pattern_names[g_pattern]);
            canvas.write_text(off, bar_y + 1, buf, S_BAR_FG);
            off += static_cast<int>(std::strlen(buf));

            if (g_paused) {
                canvas.write_text(off, bar_y + 1, "  \u23f8 PAUSED", S_BAR_ACCENT);
                off += 10;
            }

            // Timer on right
            std::snprintf(buf, sizeof(buf), "%d:%02d ", secs / 60, secs % 60);
            int timer_x = W - static_cast<int>(std::strlen(buf));
            if (timer_x > off)
                canvas.write_text(timer_x, bar_y + 1, buf, S_DIM);

            // Title on right of top status line
            canvas.write_text(W - 21, bar_y, " SORTING VISUALIZER ", S_BAR_ACCENT);

            // Content area
            int content_h = H - 2;
            if (content_h < 6) return;

            if (g_solo >= 0 && g_solo < NUM_ALGOS) {
                draw_panel(canvas, 0, 0, W, content_h, g_sorts[g_solo], ARRAY_SIZE);
            } else {
                // Dynamic grid layout for 8 algorithms
                // Prefer 4x2 (4 columns, 2 rows) for wide terminals,
                // 2x4 for narrow/tall terminals
                int cols, rows;
                if (W >= content_h * 3) {
                    cols = 4; rows = 2;
                } else if (W >= content_h * 2) {
                    cols = 4; rows = 2;
                } else {
                    cols = 2; rows = 4;
                }

                int cell_w = W / cols;
                int cell_h = content_h / rows;

                for (int i = 0; i < NUM_ALGOS; ++i) {
                    int col = i % cols;
                    int row = i / cols;
                    int px = col * cell_w;
                    int py = row * cell_h;
                    int pw = (col == cols - 1) ? (W - px) : cell_w;
                    int ph = (row == rows - 1) ? (content_h - py) : cell_h;
                    draw_panel(canvas, px, py, pw, ph, g_sorts[i], ARRAY_SIZE);
                }
            }
        }
    );
}
