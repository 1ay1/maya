// maya -- Sorting Algorithm Visualizer
//
// Four sorting algorithms race side-by-side: Bubble Sort, Quick Sort,
// Merge Sort, and Heap Sort. Colored bars represent array values with
// a rainbow gradient. Bars flash green for comparisons, red for swaps,
// and gold when placed in final sorted position. A celebration wave
// plays when each algorithm finishes.
//
// Keys: q/Esc=quit  space=restart  1-4=solo algorithm

#include <maya/internal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya;

// -- Constants ---------------------------------------------------------------

static constexpr int NUM_ALGOS = 4;
static constexpr int ARRAY_SIZE = 64;
static constexpr int CELEBRATION_FRAMES = 40;

// -- Color helpers -----------------------------------------------------------

// Rainbow gradient: value 0..max -> blue -> cyan -> green -> yellow -> red -> magenta
static Color value_color(int val, int max_val) {
    float t = (max_val > 0) ? static_cast<float>(val) / max_val : 0.f;
    // HSV-like rainbow: hue from 240 (blue) down to 0 (red) then to 300 (magenta)
    float hue = (1.f - t) * 240.f + t * 300.f;
    // wrap hue and convert to RGB
    float h = std::fmod(hue, 360.f) / 60.f;
    float c = 1.f, x = 1.f - std::fabs(std::fmod(h, 2.f) - 1.f);
    float r = 0, g = 0, b = 0;
    if (h < 1)      { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    return Color::rgb(
        static_cast<uint8_t>(r * 220 + 35),
        static_cast<uint8_t>(g * 220 + 35),
        static_cast<uint8_t>(b * 220 + 35));
}

// -- Sorting state -----------------------------------------------------------

enum class HighlightKind { None, Compare, Swap, Sorted };

struct SortState {
    const char* name;
    std::vector<int> arr;
    int comparisons = 0;
    int swaps = 0;
    bool done = false;
    int celebration_frame = 0;

    // Per-element highlight for current frame
    std::vector<HighlightKind> highlight;

    // Coroutine-style state: we record pending operations as a queue
    struct Op {
        enum Type { COMPARE, SWAP, MARK_SORTED } type;
        int i, j; // indices
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

    // Advance one operation
    void step() {
        // Clear previous highlights (except Sorted)
        for (auto& h : highlight)
            if (h != HighlightKind::Sorted) h = HighlightKind::None;

        if (op_idx >= ops.size()) {
            if (!done) {
                done = true;
                celebration_frame = 0;
                // Mark all as sorted
                for (auto& h : highlight) h = HighlightKind::Sorted;
            }
            return;
        }

        auto& op = ops[op_idx++];
        switch (op.type) {
            case Op::COMPARE:
                ++comparisons;
                if (op.i >= 0 && op.i < (int)arr.size()) highlight[op.i] = HighlightKind::Compare;
                if (op.j >= 0 && op.j < (int)arr.size()) highlight[op.j] = HighlightKind::Compare;
                break;
            case Op::SWAP:
                ++swaps;
                if (op.i >= 0 && op.i < (int)arr.size() &&
                    op.j >= 0 && op.j < (int)arr.size()) {
                    std::swap(arr[op.i], arr[op.j]);
                    highlight[op.i] = HighlightKind::Swap;
                    highlight[op.j] = HighlightKind::Swap;
                }
                break;
            case Op::MARK_SORTED:
                if (op.i >= 0 && op.i < (int)arr.size()) highlight[op.i] = HighlightKind::Sorted;
                break;
        }
    }
};

// -- Pre-generate operation lists for each algorithm -------------------------

static void gen_bubble_sort(SortState& s) {
    // Simulate bubble sort, recording ops
    std::vector<int> a = s.arr;
    int n = (int)a.size();
    for (int i = 0; i < n - 1; ++i) {
        for (int j = 0; j < n - 1 - i; ++j) {
            s.ops.push_back({SortState::Op::COMPARE, j, j + 1});
            if (a[j] > a[j + 1]) {
                std::swap(a[j], a[j + 1]);
                s.ops.push_back({SortState::Op::SWAP, j, j + 1});
            }
        }
        s.ops.push_back({SortState::Op::MARK_SORTED, n - 1 - i, 0});
    }
    s.ops.push_back({SortState::Op::MARK_SORTED, 0, 0});
}

static void gen_quicksort(SortState& s) {
    std::vector<int> a = s.arr;
    int n = (int)a.size();

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

        // Push larger partition first for better stack behavior
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
    std::vector<int> a = s.arr;
    int n = (int)a.size();

    // Bottom-up merge sort
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = std::min(lo + width, n);
            int hi = std::min(lo + 2 * width, n);

            // Merge [lo..mid) and [mid..hi)
            std::vector<int> tmp;
            int i = lo, j = mid;
            while (i < mid && j < hi) {
                s.ops.push_back({SortState::Op::COMPARE, i, j});
                if (a[i] <= a[j]) {
                    tmp.push_back(a[i++]);
                } else {
                    tmp.push_back(a[j++]);
                }
            }
            while (i < mid) tmp.push_back(a[i++]);
            while (j < hi) tmp.push_back(a[j++]);

            // Copy back with swap visualization
            for (int k = 0; k < (int)tmp.size(); ++k) {
                if (a[lo + k] != tmp[k]) {
                    a[lo + k] = tmp[k];
                    s.ops.push_back({SortState::Op::SWAP, lo + k, lo + k});
                }
            }
        }
    }
    // Mark all sorted
    for (int i = 0; i < n; ++i)
        s.ops.push_back({SortState::Op::MARK_SORTED, i, 0});
}

static void gen_heap_sort(SortState& s) {
    std::vector<int> a = s.arr;
    int n = (int)a.size();

    // Sift down
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

    // Build max heap
    for (int start = (n - 2) / 2; start >= 0; --start)
        sift_down(start, n - 1);

    // Extract elements
    for (int end = n - 1; end > 0; --end) {
        std::swap(a[0], a[end]);
        s.ops.push_back({SortState::Op::SWAP, 0, end});
        s.ops.push_back({SortState::Op::MARK_SORTED, end, 0});
        sift_down(0, end - 1);
    }
    s.ops.push_back({SortState::Op::MARK_SORTED, 0, 0});
}

// -- Global state ------------------------------------------------------------

static std::mt19937 g_rng{42};
static SortState g_sorts[NUM_ALGOS];
static int g_solo = -1; // -1 = show all, 0-3 = solo one
static int g_frame = 0;

// Style caches
static uint16_t S_BG;
static uint16_t S_BAR_BG;
static uint16_t S_BAR_DIM;
static uint16_t S_BAR_TITLE;
static uint16_t S_COMPARE_BG;
static uint16_t S_SWAP_BG;
static uint16_t S_SORTED_BG;
static uint16_t S_BORDER;
static uint16_t S_DONE;
static uint16_t S_TITLE_RIGHT;

// Pre-interned styles: bg = value_color for each array value
static uint16_t S_VAL_BAR[ARRAY_SIZE];

// Celebration palette: 72 hues, pre-interned as bg styles
static constexpr int CELEB_HUES = 72;
static uint16_t S_CELEB[CELEB_HUES];

static void init_data() {
    std::vector<int> base(ARRAY_SIZE);
    for (int i = 0; i < ARRAY_SIZE; ++i) base[i] = i + 1;
    std::shuffle(base.begin(), base.end(), g_rng);

    g_sorts[0].name = "Bubble Sort";
    g_sorts[1].name = "Quick Sort";
    g_sorts[2].name = "Merge Sort";
    g_sorts[3].name = "Heap Sort";

    for (int i = 0; i < NUM_ALGOS; ++i) {
        g_sorts[i].reset(base);
    }

    gen_bubble_sort(g_sorts[0]);
    gen_quicksort(g_sorts[1]);
    gen_merge_sort(g_sorts[2]);
    gen_heap_sort(g_sorts[3]);

    g_frame = 0;
}

// -- Render helpers ----------------------------------------------------------

static uint16_t style_for_bar(int val, HighlightKind hk, bool celebrating,
                              int celeb_frame, int bar_index) {
    // During celebration wave, use shifting rainbow palette
    if (celebrating && celeb_frame > 0 && celeb_frame <= CELEBRATION_FRAMES) {
        float wave = std::sin((float)bar_index * 0.3f - (float)celeb_frame * 0.25f);
        if (wave > 0.f) {
            int hue_idx = ((bar_index * 7 + celeb_frame * 9) % 360);
            // Map 0..359 -> 0..71
            int ci = (hue_idx * CELEB_HUES) / 360;
            return S_CELEB[std::clamp(ci, 0, CELEB_HUES - 1)];
        }
    }

    // Highlight overrides
    if (!celebrating || celeb_frame == 0) {
        if (hk == HighlightKind::Compare) return S_COMPARE_BG;
        if (hk == HighlightKind::Swap)    return S_SWAP_BG;
        if (hk == HighlightKind::Sorted)  return S_SORTED_BG;
    }

    // Normal rainbow bar
    int idx = std::clamp(val - 1, 0, ARRAY_SIZE - 1);
    return S_VAL_BAR[idx];
}

static void draw_panel(Canvas& canvas, int px, int py, int pw, int ph,
                       SortState& s, int max_val) {
    // Panel background
    for (int y = py; y < py + ph && y >= 0; ++y)
        for (int x = px; x < px + pw; ++x)
            canvas.set(x, y, U' ', S_BG);

    // Header: name + stats (2 rows)
    {
        char buf[128];
        if (s.done) {
            std::snprintf(buf, sizeof(buf), " %s  DONE!", s.name);
        } else {
            std::snprintf(buf, sizeof(buf), " %s", s.name);
        }
        canvas.write_text(px, py, buf, s.done ? S_DONE : S_BAR_TITLE);

        std::snprintf(buf, sizeof(buf), " cmp:%-6d swp:%-6d", s.comparisons, s.swaps);
        canvas.write_text(px, py + 1, buf, S_BAR_DIM);
    }

    // Border line
    for (int x = px; x < px + pw; ++x)
        canvas.set(x, py + 2, U'\u2500', S_BORDER);

    // Bar area
    int bar_top = py + 3;
    int bar_height = ph - 3;
    if (bar_height <= 0) return;

    int n = (int)s.arr.size();
    float bar_w_f = static_cast<float>(pw) / n;

    for (int i = 0; i < n; ++i) {
        int bx = px + static_cast<int>(i * bar_w_f);
        int bw = std::max(1, static_cast<int>((i + 1) * bar_w_f) - static_cast<int>(i * bar_w_f));

        int val = s.arr[i];
        int bh = static_cast<int>(static_cast<float>(val) / max_val * bar_height);
        if (bh < 1) bh = 1;

        uint16_t sid = style_for_bar(val, s.highlight[i], s.done,
                                     s.celebration_frame, i);

        // Draw bars from bottom up
        for (int dy = 0; dy < bh && dy < bar_height; ++dy) {
            int y = bar_top + bar_height - 1 - dy;
            for (int dx = 0; dx < bw; ++dx) {
                int x = bx + dx;
                if (x < px + pw)
                    canvas.set(x, y, U' ', sid);
            }
        }
    }
}

// -- Main --------------------------------------------------------------------

int main() {
    init_data();

    // How many ops to process per frame (we batch a few for speed)
    static int ops_per_frame = 3;

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .title = "sorting visualizer"},

        // on_resize
        [](StylePool& pool, int w, int h) {
            (void)w; (void)h;
            S_BG         = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)));
            S_BAR_BG     = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 35)).with_fg(Color::rgb(140, 140, 160)));
            S_BAR_DIM    = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)).with_fg(Color::rgb(100, 100, 120)));
            S_BAR_TITLE  = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)).with_fg(Color::rgb(180, 220, 255)).with_bold());
            S_COMPARE_BG = pool.intern(Style{}.with_bg(Color::rgb(50, 255, 100)));
            S_SWAP_BG    = pool.intern(Style{}.with_bg(Color::rgb(255, 60, 60)));
            S_SORTED_BG  = pool.intern(Style{}.with_bg(Color::rgb(255, 215, 0)));
            S_BORDER     = pool.intern(Style{}.with_fg(Color::rgb(60, 60, 80)).with_bg(Color::rgb(15, 15, 25)));
            S_DONE       = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)).with_fg(Color::rgb(100, 255, 150)).with_bold());
            S_TITLE_RIGHT = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 35)).with_fg(Color::rgb(255, 180, 80)).with_bold());

            // Rainbow bar styles (bg = value color)
            for (int i = 0; i < ARRAY_SIZE; ++i) {
                Color c = value_color(i + 1, ARRAY_SIZE);
                S_VAL_BAR[i] = pool.intern(Style{}.with_bg(c));
            }

            // Celebration palette
            for (int i = 0; i < CELEB_HUES; ++i) {
                float hue = static_cast<float>(i) * 360.f / CELEB_HUES;
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

            on(ev, ' ', [] {
                g_rng.seed(static_cast<unsigned>(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
                init_data();
            });

            on(ev, '1', [] { g_solo = (g_solo == 0) ? -1 : 0; });
            on(ev, '2', [] { g_solo = (g_solo == 1) ? -1 : 1; });
            on(ev, '3', [] { g_solo = (g_solo == 2) ? -1 : 2; });
            on(ev, '4', [] { g_solo = (g_solo == 3) ? -1 : 3; });

            return true;
        },

        // on_paint
        [](Canvas& canvas, int W, int H) {
            ++g_frame;

            // Fill background
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    canvas.set(x, y, U' ', S_BG);

            // Step sorting algorithms
            for (int i = 0; i < NUM_ALGOS; ++i) {
                auto& s = g_sorts[i];
                if (!s.done) {
                    for (int k = 0; k < ops_per_frame; ++k)
                        s.step();
                } else if (s.celebration_frame <= CELEBRATION_FRAMES) {
                    ++s.celebration_frame;
                }
            }

            // Status bar at bottom
            int bar_y = H - 1;
            for (int x = 0; x < W; ++x)
                canvas.set(x, bar_y, U' ', S_BAR_BG);

            {
                const char* help = " [space] restart  [1-4] solo  [q] quit";
                canvas.write_text(0, bar_y, help, S_BAR_BG);

                // Title on right
                const char* title = "SORTING VISUALIZER ";
                int tlen = (int)std::strlen(title);
                if (tlen < W)
                    canvas.write_text(W - tlen, bar_y, title, S_TITLE_RIGHT);
            }

            int content_h = H - 1; // rows available for panels

            if (g_solo >= 0 && g_solo < NUM_ALGOS) {
                // Solo mode: one algorithm fills the screen
                draw_panel(canvas, 0, 0, W, content_h, g_sorts[g_solo], ARRAY_SIZE);
            } else {
                // 2x2 grid layout
                int half_w = W / 2;
                int half_h = content_h / 2;

                draw_panel(canvas, 0,      0,      half_w, half_h, g_sorts[0], ARRAY_SIZE);
                draw_panel(canvas, half_w,  0,      W - half_w, half_h, g_sorts[1], ARRAY_SIZE);
                draw_panel(canvas, 0,      half_h, half_w, content_h - half_h, g_sorts[2], ARRAY_SIZE);
                draw_panel(canvas, half_w, half_h, W - half_w, content_h - half_h, g_sorts[3], ARRAY_SIZE);
            }
        }
    );
}
