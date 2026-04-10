// music.cpp — Terminal music player with animated visualizations
//
// Uses maya::run() with fps=15 for continuous rendering.
// A Spotify/Apple Music-inspired terminal music player with animated
// heatmap album art, sparkline audio visualizer, progress bar, and
// scrollable playlist. All data is simulated.
//
// Controls:
//   space       play/pause
//   n           next track
//   p           previous track
//   s           toggle shuffle
//   r           cycle repeat (off/one/all)
//   +/=         volume up
//   -           volume down
//   j/↓         scroll playlist down
//   k/↑         scroll playlist up
//   q/Esc       quit
//
// Usage:  ./maya_music

#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/heatmap.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/sparkline.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// -- Helpers -----------------------------------------------------------------

static std::mt19937 rng{std::random_device{}()};
static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

static maya::Style fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}

// -- Track data --------------------------------------------------------------

struct Track {
    std::string title;
    std::string artist;
    std::string genre;
    float duration;       // seconds (real time = duration / speed)
    float freq_base;      // base frequency for visualizer signature
    float freq_mod;       // modulation rate
    uint8_t art_r1, art_g1, art_b1;  // heatmap low color
    uint8_t art_r2, art_g2, art_b2;  // heatmap high color
};

static std::vector<Track> tracks = {
    {"Neon Dreams",          "Synthwave Collective", "Synthwave",    15.0f, 2.0f, 1.5f,   20,  0, 60,    0, 255,200},
    {"Binary Sunset",        "The Algorithms",       "Ambient",      15.0f, 1.2f, 0.8f,   10, 10, 40,  255,140, 50},
    {"Stack Overflow",       "Debug Mode",           "Electronic",   15.0f, 3.5f, 2.2f,   40,  5, 10,  255, 60, 80},
    {"Quantum Entangled",    "Hadron",               "IDM",          15.0f, 4.0f, 3.0f,   15,  0, 40,  180, 80,255},
    {"Hello World",          "printf()",             "Chiptune",     15.0f, 5.0f, 4.0f,    5, 30,  5,   80,255, 80},
    {"Segmentation Fault",   "Core Dump",            "Industrial",   15.0f, 1.8f, 1.0f,   30, 10, 10,  255, 30, 30},
    {"Recursive Dreams",     "Lambda Express",       "Downtempo",    15.0f, 0.8f, 0.5f,    5,  5, 30,  100,180,255},
    {"Kernel Panic",         "Ring Zero",            "Darkwave",     15.0f, 2.5f, 1.8f,   25,  0, 25,  200,  0,100},
    {"Garbage Collector",    "Heap Alloc",           "Techno",       15.0f, 6.0f, 3.5f,   10, 10, 10,    0,200,180},
    {"Undefined Behavior",   "Volatile Memory",      "Glitch",       15.0f, 7.0f, 5.0f,   20, 20,  0,  255,255,  0},
    {"git push --force",     "Merge Conflict",       "Punk",         15.0f, 4.5f, 2.8f,   30,  5,  0,  255,120,  0},
    {"Async Await",          "Event Loop",           "House",        15.0f, 3.0f, 2.0f,    0, 10, 30,   60,160,255},
    {"Deep Learning Blues",  "Neural Net",           "Blues",        15.0f, 1.5f, 0.7f,   10,  5, 25,   80,120,220},
    {"Pointer Arithmetic",   "Memory Leak",          "Math Rock",    15.0f, 5.5f, 3.8f,   15, 15,  5,  220,200,100},
    {"404 Not Found",        "HTTP Response",        "Lo-Fi",        15.0f, 1.0f, 0.6f,    8,  8, 20,  150,150,180},
};

// -- State -------------------------------------------------------------------

static int current_track = 0;
static float progress = 0.0f;      // 0..1
static bool playing = true;
static bool shuffle_on = false;
static int repeat_mode = 0;         // 0=off, 1=one, 2=all
static float volume = 0.75f;
static int playlist_scroll = 0;
static int frame = 0;

// Visualizer data
static constexpr int VIS_BINS = 32;
static std::array<float, VIS_BINS> vis_left{};
static std::array<float, VIS_BINS> vis_right{};

// Shuffle order
static std::vector<int> shuffle_order;

static void init_shuffle() {
    shuffle_order.resize(tracks.size());
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        shuffle_order[static_cast<size_t>(i)] = i;
    std::shuffle(shuffle_order.begin(), shuffle_order.end(), rng);
}

static void advance_track(int direction) {
    if (shuffle_on && direction == 1) {
        // Find current in shuffle order and go next
        for (int i = 0; i < static_cast<int>(shuffle_order.size()); ++i) {
            if (shuffle_order[static_cast<size_t>(i)] == current_track) {
                int next = (i + 1) % static_cast<int>(shuffle_order.size());
                current_track = shuffle_order[static_cast<size_t>(next)];
                progress = 0.0f;
                return;
            }
        }
        current_track = shuffle_order[0];
    } else {
        current_track += direction;
        if (current_track >= static_cast<int>(tracks.size())) {
            current_track = (repeat_mode == 2) ? 0 : static_cast<int>(tracks.size()) - 1;
        }
        if (current_track < 0) current_track = 0;
    }
    progress = 0.0f;
}

// -- Tick --------------------------------------------------------------------

static void tick(float dt) {
    frame++;
    if (!playing) return;

    auto& trk = tracks[static_cast<size_t>(current_track)];
    progress += dt / trk.duration;

    if (progress >= 1.0f) {
        progress = 0.0f;
        if (repeat_mode == 1) {
            // repeat one: stay on same track
        } else {
            advance_track(1);
        }
    }

    // Update visualizer bins based on track's frequency signature
    float t = static_cast<float>(frame) / 15.0f;
    for (int i = 0; i < VIS_BINS; ++i) {
        float fi = static_cast<float>(i) / static_cast<float>(VIS_BINS);
        float sig = std::sin(t * trk.freq_base + fi * 6.28f) * 0.3f
                  + std::sin(t * trk.freq_mod * 2.0f + fi * 12.56f) * 0.2f
                  + randf(-0.15f, 0.15f);
        float energy = 0.5f + sig;
        energy *= volume;
        energy = std::clamp(energy, 0.05f, 1.0f);

        // Smooth decay
        vis_left[static_cast<size_t>(i)] += (energy - vis_left[static_cast<size_t>(i)]) * 0.4f;

        float sig2 = std::sin(t * trk.freq_base * 1.1f + fi * 6.28f + 1.0f) * 0.3f
                    + std::cos(t * trk.freq_mod * 1.8f + fi * 12.56f) * 0.2f
                    + randf(-0.15f, 0.15f);
        float energy2 = 0.5f + sig2;
        energy2 *= volume;
        energy2 = std::clamp(energy2, 0.05f, 1.0f);
        vis_right[static_cast<size_t>(i)] += (energy2 - vis_right[static_cast<size_t>(i)]) * 0.4f;
    }
}

// -- Format helpers ----------------------------------------------------------

static std::string fmt_time(float seconds) {
    int total = static_cast<int>(seconds);
    int m = total / 60;
    int s = total % 60;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

static std::string block_bar(float v, int width) {
    int filled = std::clamp(static_cast<int>(v * static_cast<float>(width)), 0, width);
    std::string s;
    for (int i = 0; i < filled; ++i) s += "\xe2\x96\x88"; // █
    for (int i = filled; i < width; ++i) s += "\xe2\x94\x80"; // ─
    return s;
}

// -- Badge theme helper ------------------------------------------------------

static maya::Badge genre_badge(const std::string& genre) {
    if (genre == "Synthwave" || genre == "Electronic" || genre == "Techno" || genre == "House")
        return maya::Badge::info(genre);
    if (genre == "Ambient" || genre == "Downtempo" || genre == "Lo-Fi")
        return maya::Badge::success(genre);
    if (genre == "Industrial" || genre == "Darkwave" || genre == "Punk")
        return maya::Badge::error(genre);
    if (genre == "IDM" || genre == "Glitch" || genre == "Math Rock")
        return maya::Badge::warning(genre);
    return maya::Badge::tool(genre);
}

// -- UI Builders -------------------------------------------------------------

static maya::Element build_now_playing() {
    auto& trk = tracks[static_cast<size_t>(current_track)];

    auto status_icon = playing ? "▶" : "⏸";
    auto status_style = playing ? fg_rgb(0, 220, 120) : fg_rgb(255, 200, 60);

    return (h(
        text(std::string(status_icon), status_style) | w_<2>,
        text(trk.title, fg_rgb(255, 255, 255).with_bold()),
        text("  "),
        text(trk.artist) | Fg<170, 170, 190>,
        text("  "),
        genre_badge(trk.genre).build(),
        space,
        text(fmt_time(progress * trk.duration) + " / " + fmt_time(trk.duration)) | Dim
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_album_art() {
    auto& trk = tracks[static_cast<size_t>(current_track)];
    float t = static_cast<float>(frame) / 15.0f;

    constexpr int rows = 6;
    constexpr int cols = 20;
    std::vector<std::vector<float>> data(rows, std::vector<float>(cols));

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            float fr = static_cast<float>(r) / static_cast<float>(rows);
            float fc = static_cast<float>(c) / static_cast<float>(cols);
            float v = std::sin(fc * trk.freq_base * 3.14f + t * trk.freq_mod) * 0.3f
                    + std::cos(fr * trk.freq_mod * 3.14f + t * trk.freq_base * 0.7f) * 0.3f
                    + std::sin((fr + fc) * 4.0f + t * 1.5f) * 0.2f
                    + 0.5f;
            v = std::clamp(v, 0.0f, 1.0f);
            data[static_cast<size_t>(r)][static_cast<size_t>(c)] = v;
        }
    }

    maya::Heatmap hm(std::move(data));
    hm.set_low_color(maya::Color::rgb(trk.art_r1, trk.art_g1, trk.art_b1));
    hm.set_high_color(maya::Color::rgb(trk.art_r2, trk.art_g2, trk.art_b2));

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(trk.art_r2 / 2, trk.art_g2 / 2, trk.art_b2 / 2))
        .border_text(" Album Art ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(hm.build());
}

static maya::Element build_progress() {
    auto& trk = tracks[static_cast<size_t>(current_track)];

    maya::ProgressBar bar(maya::ProgressConfig{
        .width = 0,
        .fill_color = maya::Color::rgb(trk.art_r2, trk.art_g2, trk.art_b2),
        .bg_color = maya::Color::rgb(40, 42, 50),
        .show_track = true,
        .show_percentage = false,
    });
    bar.set(progress);
    bar.set_label(fmt_time(progress * trk.duration) + " / " + fmt_time(trk.duration));

    return (v(bar.build()) | padding(0, 1, 0, 1)).build();
}

static maya::Element build_controls() {
    auto& trk = tracks[static_cast<size_t>(current_track)];
    auto accent = maya::Color::rgb(trk.art_r2, trk.art_g2, trk.art_b2);

    auto ctrl = [&](const std::string& icon, bool active) {
        if (active)
            return text(icon, fg_rgb(trk.art_r2, trk.art_g2, trk.art_b2).with_bold());
        return text(icon) | Dim;
    };

    std::string repeat_label = repeat_mode == 0 ? "off" : (repeat_mode == 1 ? "one" : "all");

    return (h(
        text("    "),
        ctrl("\xe2\x8f\xae", false),  // ⏮
        text("  "),
        ctrl(playing ? "\xe2\x96\xb6" : "\xe2\x8f\xb8", true),  // ▶ or ⏸
        text("  "),
        ctrl("\xe2\x8f\xad", false),  // ⏭
        text("    "),
        ctrl("\xf0\x9f\x94\x80", shuffle_on),  // 🔀
        text(" "),
        text(shuffle_on ? "on" : "off", shuffle_on ? fg_rgb(0, 220, 120) : fg_rgb(80, 80, 100)),
        text("    "),
        ctrl("\xf0\x9f\x94\x81", repeat_mode > 0),  // 🔁
        text(" "),
        text(repeat_label, repeat_mode > 0 ? fg_rgb(0, 220, 120) : fg_rgb(80, 80, 100)),
        space
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_playlist() {
    std::vector<maya::Element> rows;

    // Header
    rows.push_back((h(
        t<"#"> | Bold | Dim | w_<4>,
        t<"TITLE"> | Bold | Dim | w_<24>,
        t<"ARTIST"> | Bold | Dim | w_<22>,
        t<"TIME"> | Bold | Dim | w_<6>
    ) | gap_<1>).build());

    int visible = 8;
    // Ensure current track is visible
    if (current_track < playlist_scroll) playlist_scroll = current_track;
    if (current_track >= playlist_scroll + visible) playlist_scroll = current_track - visible + 1;
    playlist_scroll = std::clamp(playlist_scroll, 0,
        std::max(0, static_cast<int>(tracks.size()) - visible));

    for (int i = playlist_scroll; i < std::min(playlist_scroll + visible, static_cast<int>(tracks.size())); ++i) {
        auto& trk = tracks[static_cast<size_t>(i)];
        bool is_current = (i == current_track);

        auto num_style = is_current ? fg_rgb(trk.art_r2, trk.art_g2, trk.art_b2).with_bold()
                                    : fg_rgb(80, 80, 100);
        auto title_style = is_current ? fg_rgb(255, 255, 255).with_bold()
                                      : fg_rgb(190, 190, 200);
        auto artist_style = is_current ? fg_rgb(trk.art_r2, trk.art_g2, trk.art_b2)
                                       : fg_rgb(120, 120, 140);

        std::string num_str = is_current ? " \xe2\x96\xb6 " : " " + std::to_string(i + 1) + " ";

        rows.push_back((h(
            text(num_str, num_style) | w_<4>,
            text(trk.title, title_style) | w_<24>,
            text(trk.artist, artist_style) | w_<22>,
            text(fmt_time(trk.duration)) | Dim | w_<6>
        ) | gap_<1>).build());
    }

    // Scroll indicator
    if (static_cast<int>(tracks.size()) > visible) {
        int total = static_cast<int>(tracks.size());
        std::string indicator = std::to_string(playlist_scroll + 1) + "-"
            + std::to_string(std::min(playlist_scroll + visible, total))
            + " of " + std::to_string(total);
        rows.push_back((h(space, text(indicator) | Dim)).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" Playlist ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_visualizer() {
    std::vector<float> left_data(vis_left.begin(), vis_left.end());
    std::vector<float> right_data(vis_right.begin(), vis_right.end());

    auto& trk = tracks[static_cast<size_t>(current_track)];

    maya::Sparkline left_spark(std::move(left_data), maya::SparklineConfig{
        .color = maya::Color::rgb(trk.art_r2, trk.art_g2, trk.art_b2),
    });
    left_spark.set_label("L");
    left_spark.set_min(0.0f);
    left_spark.set_max(1.0f);

    maya::Sparkline right_spark(std::move(right_data), maya::SparklineConfig{
        .color = maya::Color::rgb(
            static_cast<uint8_t>(std::min(255, trk.art_r2 + 40)),
            static_cast<uint8_t>(std::min(255, trk.art_g2 + 40)),
            static_cast<uint8_t>(std::min(255, trk.art_b2 + 40))),
    });
    right_spark.set_label("R");
    right_spark.set_min(0.0f);
    right_spark.set_max(1.0f);

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" Visualizer ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(
            left_spark.build(),
            right_spark.build()
        );
}

static maya::Element build_queue() {
    std::vector<maya::Element> rows;

    rows.push_back((text("Up Next") | Bold | Fg<170, 170, 190>).build());

    for (int i = 1; i <= 4; ++i) {
        int idx;
        if (shuffle_on) {
            // find current in shuffle order
            int pos = 0;
            for (int j = 0; j < static_cast<int>(shuffle_order.size()); ++j) {
                if (shuffle_order[static_cast<size_t>(j)] == current_track) {
                    pos = j;
                    break;
                }
            }
            idx = shuffle_order[static_cast<size_t>((pos + i) % static_cast<int>(shuffle_order.size()))];
        } else {
            idx = (current_track + i) % static_cast<int>(tracks.size());
        }

        auto& trk = tracks[static_cast<size_t>(idx)];
        rows.push_back((h(
            text(std::to_string(i) + ".") | Dim | w_<3>,
            text(trk.title) | Fg<200, 200, 210> | clip,
            text("  "),
            text(trk.artist) | Dim | clip
        )).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" Queue ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_status_bar() {
    auto& trk = tracks[static_cast<size_t>(current_track)];

    // Volume bar
    std::string vol_bar = block_bar(volume, 10);
    int vol_pct = static_cast<int>(volume * 100.0f);

    std::string repeat_str = repeat_mode == 0 ? "off" : (repeat_mode == 1 ? "one" : "all");

    return (h(
        text(" VOL") | Fg<140, 140, 160>,
        text(" " + vol_bar, fg_rgb(trk.art_r2, trk.art_g2, trk.art_b2)),
        text(" " + std::to_string(vol_pct) + "%") | Fg<140, 140, 160>,
        text("  |") | Fg<60, 60, 80>,
        text("  repeat:") | Fg<140, 140, 160>,
        text(repeat_str, repeat_mode > 0 ? fg_rgb(0, 220, 120) : fg_rgb(80, 80, 100)),
        text("  shuffle:") | Fg<140, 140, 160>,
        text(shuffle_on ? "on" : "off", shuffle_on ? fg_rgb(0, 220, 120) : fg_rgb(80, 80, 100)),
        space,
        text(" spc") | Bold | Fg<180, 220, 255>, text(":play") | Fg<120, 120, 140>,
        text(" n") | Bold | Fg<180, 220, 255>, text(":next") | Fg<120, 120, 140>,
        text(" p") | Bold | Fg<180, 220, 255>, text(":prev") | Fg<120, 120, 140>,
        text(" s") | Bold | Fg<180, 220, 255>, text(":shuf") | Fg<120, 120, 140>,
        text(" r") | Bold | Fg<180, 220, 255>, text(":rep") | Fg<120, 120, 140>,
        text(" +/-") | Bold | Fg<180, 220, 255>, text(":vol") | Fg<120, 120, 140>,
        text(" q") | Bold | Fg<180, 220, 255>, text(":quit ") | Fg<120, 120, 140>
    ) | pad<0, 1, 0, 1> | Bg<30, 30, 42>).build();
}

// -- Render (without tick) ---------------------------------------------------

static maya::Element render() {
    // Left column: album art + visualizer + queue
    auto left_col = (v(
        build_album_art(),
        build_visualizer(),
        build_queue()
    ) | grow(1)).build();

    // Right column: playlist
    auto right_col = (v(
        build_playlist()
    ) | grow(2)).build();

    // Main content: left | right
    auto main_content = (h(
        std::move(left_col),
        std::move(right_col)
    )).build();

    return vstack()(
        build_now_playing(),
        build_progress(),
        build_controls(),
        std::move(main_content),
        build_status_bar()
    );
}

// -- Main --------------------------------------------------------------------

int main() {
    init_shuffle();

    maya::run(
        {.title = "music", .fps = 15, .mode = Mode::Fullscreen},
        [](const Event& ev) {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            if (key(ev, ' '))  playing = !playing;
            if (key(ev, 'n'))  advance_track(1);
            if (key(ev, 'p'))  advance_track(-1);
            if (key(ev, 's'))  { shuffle_on = !shuffle_on; if (shuffle_on) init_shuffle(); }
            if (key(ev, 'r'))  repeat_mode = (repeat_mode + 1) % 3;
            if (key(ev, '+') || key(ev, '=')) volume = std::min(1.0f, volume + 0.05f);
            if (key(ev, '-'))  volume = std::max(0.0f, volume - 0.05f);
            if (key(ev, 'j') || key(ev, SpecialKey::Down))
                playlist_scroll = std::min(playlist_scroll + 1,
                    std::max(0, static_cast<int>(tracks.size()) - 8));
            if (key(ev, 'k') || key(ev, SpecialKey::Up))
                playlist_scroll = std::max(0, playlist_scroll - 1);
            return true;
        },
        [] {
            tick(1.0f / 15.0f);
            return render();
        }
    );
}
