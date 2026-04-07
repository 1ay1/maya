// stocks.cpp — Live stock ticker dashboard (inline mode)
//
// A visually rich terminal dashboard with animated charts, sparklines,
// color-coded gains/losses, and a scrolling news feed. Uses maya::run
// with alt_screen=false so output stays in terminal scrollback.
//
// All data is simulated with correlated random walks.
//
// Controls:
//   ↑/↓  k/j    select stock
//   ←/→  h/l    change timeframe (1m 5m 15m 1h 1d)
//   r           refresh / randomize market event
//   space       toggle market hours (open/closed)
//   q/Esc       quit
//
// Usage:  ./maya_stocks

#include <maya/dsl.hpp>
#include <maya/app/run.hpp>
#include <maya/app/events.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace maya::dsl;

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::mt19937 rng{std::random_device{}()};
static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}
static int randi(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

static maya::Style fg(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}

static maya::Style gain_style(float v) {
    if (v > 0) return fg(0, 220, 100).with_bold();
    if (v < 0) return fg(255, 60, 70).with_bold();
    return fg(120, 120, 130);
}

// ── Braille chart ───────────────────────────────────────────────────────────
// Renders a line chart using braille dots (2×4 grid per character = 2 cols × 4 rows).
// Each braille character encodes 8 dots. We use a simplified 1-row-per-char approach.

static const char* spark_chars[] = {"▁","▂","▃","▄","▅","▆","▇","█"};

static std::string spark_line(const std::vector<float>& data, int width) {
    if (data.empty()) return "";
    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());
    float range = mx - mn;
    if (range < 0.001f) range = 1.0f;

    std::string out;
    int step = std::max(1, static_cast<int>(data.size()) / width);
    for (int i = 0; i < width && i * step < static_cast<int>(data.size()); ++i) {
        float v = data[static_cast<size_t>(i * step)];
        int idx = std::clamp(static_cast<int>((v - mn) / range * 7.0f), 0, 7);
        out += spark_chars[idx];
    }
    return out;
}

// Big chart using braille (2x4 dot matrix per cell)
static std::vector<std::string> braille_chart(const std::vector<float>& data, int width, int height) {
    if (data.empty()) return std::vector<std::string>(static_cast<size_t>(height), "");

    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());
    float range = mx - mn;
    if (range < 0.001f) { mn -= 1; mx += 1; range = 2; }

    // Braille: each cell is 2 cols × 4 rows of dots
    int dot_rows = height * 4;
    int dot_cols = width * 2;

    // Map data points to dot columns
    std::vector<int> dot_y(static_cast<size_t>(dot_cols), 0);
    for (int dx = 0; dx < dot_cols; ++dx) {
        int di = static_cast<int>(static_cast<float>(dx) / static_cast<float>(dot_cols) * static_cast<float>(data.size()));
        di = std::clamp(di, 0, static_cast<int>(data.size()) - 1);
        float v = data[static_cast<size_t>(di)];
        dot_y[static_cast<size_t>(dx)] = std::clamp(
            static_cast<int>((v - mn) / range * static_cast<float>(dot_rows - 1)),
            0, dot_rows - 1);
    }

    // Braille dot offsets: dots[col][row] where col=0,1 row=0..3
    // Unicode braille: U+2800 + sum of dot bits
    // Dot positions:  col0: 0x01,0x02,0x04,0x40  col1: 0x08,0x10,0x20,0x80
    static constexpr uint8_t dot_bits[2][4] = {
        {0x40, 0x04, 0x02, 0x01},  // col 0, rows top→bottom
        {0x80, 0x20, 0x10, 0x08},  // col 1, rows top→bottom
    };

    std::vector<std::string> rows(static_cast<size_t>(height));
    for (int cy = 0; cy < height; ++cy) {
        std::string& row = rows[static_cast<size_t>(cy)];
        for (int cx = 0; cx < width; ++cx) {
            uint8_t bits = 0;
            for (int dc = 0; dc < 2; ++dc) {
                int dx = cx * 2 + dc;
                if (dx >= dot_cols) continue;
                int dy = dot_y[static_cast<size_t>(dx)];
                // Convert data y (0=bottom) to cell row (0=top)
                int cell_top_row = (height - 1 - cy) * 4;
                for (int dr = 0; dr < 4; ++dr) {
                    int abs_row = cell_top_row + (3 - dr);
                    if (dy == abs_row) bits |= dot_bits[dc][dr];
                    // Fill below the line for area effect
                    if (abs_row <= dy && abs_row >= dy - 1) bits |= dot_bits[dc][dr];
                }
            }
            // Encode as UTF-8 (U+2800 + bits = 0xE2 0xA0 0x80+bits for bits<0x40,
            // but braille range is U+2800-U+28FF)
            char32_t cp = 0x2800 + bits;
            if (cp < 0x800) {
                row += static_cast<char>(0xC0 | (cp >> 6));
                row += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                row += static_cast<char>(0xE0 | (cp >> 12));
                row += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                row += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }
    return rows;
}

// ── Market Data ─────────────────────────────────────────────────────────────

struct Stock {
    std::string symbol;
    std::string name;
    float price;
    float open;
    float prev_close;
    float day_high;
    float day_low;
    float volume;  // in millions
    float volatility;
    std::vector<float> history;   // price history (500 ticks)
    std::vector<float> vol_hist;  // volume history
    float momentum = 0;           // drift direction
};

struct NewsItem {
    std::string source;
    std::string headline;
    int sentiment;  // -1, 0, 1
    float age;      // seconds ago
};

// ── State ───────────────────────────────────────────────────────────────────

static std::vector<Stock> stocks;
static std::vector<NewsItem> news;
static int selected = 0;
static int timeframe = 2;  // 0=1m, 1=5m, 2=15m, 3=1h, 4=1d
static const char* tf_labels[] = {"1m", "5m", "15m", "1h", "1d"};
static const int tf_points[] = {60, 120, 200, 350, 500};
static bool market_open = true;
static float elapsed = 0;
static int frame = 0;

static const char* spinners[] = {"◐","◓","◑","◒"};

// ── Init ────────────────────────────────────────────────────────────────────

static void init() {
    stocks = {
        {"AAPL",  "Apple Inc.",           189.84f, 188.50f, 187.20f, 191.30f, 187.10f, 62.4f, 0.012f, {}, {}, 0.3f},
        {"NVDA",  "NVIDIA Corp.",         875.28f, 868.00f, 862.50f, 882.40f, 860.10f, 48.7f, 0.025f, {}, {}, 0.8f},
        {"MSFT",  "Microsoft Corp.",      415.60f, 413.20f, 412.80f, 418.90f, 411.50f, 28.3f, 0.010f, {}, {}, 0.2f},
        {"GOOGL", "Alphabet Inc.",        157.25f, 155.80f, 156.40f, 158.60f, 155.20f, 22.1f, 0.015f, {}, {}, -0.1f},
        {"AMZN",  "Amazon.com Inc.",      186.51f, 184.90f, 185.30f, 188.20f, 184.10f, 35.6f, 0.018f, {}, {}, 0.5f},
        {"TSLA",  "Tesla Inc.",           248.42f, 245.00f, 243.80f, 252.30f, 242.60f, 95.2f, 0.035f, {}, {}, -0.4f},
        {"META",  "Meta Platforms Inc.",  505.75f, 502.30f, 501.90f, 509.80f, 500.10f, 18.9f, 0.020f, {}, {}, 0.6f},
        {"AMD",   "Advanced Micro Dev.",  164.38f, 162.50f, 161.80f, 166.70f, 161.20f, 42.8f, 0.028f, {}, {}, 0.4f},
    };

    // Generate history for each stock
    for (auto& s : stocks) {
        s.history.resize(500);
        s.vol_hist.resize(500);
        float p = s.prev_close;
        for (int i = 0; i < 500; ++i) {
            p += randf(-1, 1) * s.volatility * p + s.momentum * s.volatility * p * 0.1f;
            p = std::max(p, s.prev_close * 0.85f);
            s.history[static_cast<size_t>(i)] = p;
            s.vol_hist[static_cast<size_t>(i)] = randf(0.3f, 1.0f) * s.volume;
        }
        s.price = s.history.back();
        s.day_high = *std::max_element(s.history.begin(), s.history.end());
        s.day_low = *std::min_element(s.history.begin(), s.history.end());
    }

    news = {
        {"Reuters",   "Fed signals potential rate cut in September meeting",          1, 45},
        {"Bloomberg", "NVIDIA announces next-gen Blackwell Ultra GPU architecture", 1, 120},
        {"CNBC",      "Tech sector leads S&P 500 to new all-time high",              1, 230},
        {"WSJ",       "Tesla recalls 125K vehicles over seatbelt warning system",   -1, 380},
        {"Reuters",   "Apple Vision Pro sales exceed analyst expectations",           1, 510},
        {"Bloomberg", "Semiconductor supply chain bottleneck easing globally",       1, 640},
        {"FT",        "US-China trade tensions resurface over chip exports",         -1, 890},
        {"CNBC",      "Amazon AWS revenue growth accelerates in Q3 earnings",        1, 1100},
    };
}

// ── Tick ────────────────────────────────────────────────────────────────────

static void tick(float dt) {
    elapsed += dt;
    frame++;

    if (!market_open) return;

    for (auto& s : stocks) {
        // Correlated random walk with momentum
        float drift = s.momentum * s.volatility * s.price * dt;
        float noise = randf(-1, 1) * s.volatility * s.price * std::sqrt(dt) * 3.0f;
        s.price += drift + noise;
        s.price = std::max(s.price, s.prev_close * 0.80f);

        // Occasionally shift momentum
        if (randi(0, 200) == 0) s.momentum = randf(-1.0f, 1.0f);

        // Update history (shift left, append)
        s.history.erase(s.history.begin());
        s.history.push_back(s.price);

        s.vol_hist.erase(s.vol_hist.begin());
        s.vol_hist.push_back(randf(0.2f, 1.2f) * s.volume);

        s.day_high = std::max(s.day_high, s.price);
        s.day_low = std::min(s.day_low, s.price);
    }

    // Occasionally add news
    if (randi(0, 150) == 0) {
        static const std::array<std::string, 12> headlines = {
            "Quarterly earnings beat analyst estimates by 12%",
            "New partnership announcement drives after-hours surge",
            "SEC investigation concerns weigh on share price",
            "Insider selling report triggers brief selloff",
            "Upgrade to Strong Buy from Goldman Sachs",
            "Record-breaking product launch numbers reported",
            "Supply chain disruption impacts Q4 guidance",
            "Strategic acquisition of AI startup announced",
            "Board approves $10B stock buyback program",
            "Antitrust probe announced by DOJ",
            "Market cap crosses $3 trillion milestone",
            "Key executive departure raises succession concerns",
        };
        static const std::array<std::string, 5> sources = {
            "Reuters", "Bloomberg", "CNBC", "WSJ", "FT"
        };
        int sent = randi(0, 2) - 1;
        news.insert(news.begin(), {
            sources[static_cast<size_t>(randi(0, 4))],
            stocks[static_cast<size_t>(randi(0, static_cast<int>(stocks.size()) - 1))].symbol + ": " +
                headlines[static_cast<size_t>(randi(0, 11))],
            sent, 0
        });
        if (news.size() > 8) news.pop_back();
    }

    // Age news
    for (auto& n : news) n.age += dt;
}

// ── Format helpers ──────────────────────────────────────────────────────────

static std::string fmt_price(float p) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(p));
    return buf;
}

static std::string fmt_change(float price, float ref) {
    float diff = price - ref;
    float pct = (ref > 0) ? (diff / ref * 100.0f) : 0.0f;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.2f (%+.2f%%)",
                  static_cast<double>(diff), static_cast<double>(pct));
    return buf;
}

static std::string fmt_volume(float v) {
    if (v >= 1000) { char b[16]; std::snprintf(b, sizeof(b), "%.1fB", static_cast<double>(v / 1000)); return b; }
    if (v >= 1) { char b[16]; std::snprintf(b, sizeof(b), "%.1fM", static_cast<double>(v)); return b; }
    char b[16]; std::snprintf(b, sizeof(b), "%.0fK", static_cast<double>(v * 1000)); return b;
}

static std::string fmt_time(float secs) {
    if (secs < 60) return std::to_string(static_cast<int>(secs)) + "s ago";
    if (secs < 3600) return std::to_string(static_cast<int>(secs / 60)) + "m ago";
    return std::to_string(static_cast<int>(secs / 3600)) + "h ago";
}

// ── UI Builders ─────────────────────────────────────────────────────────────

static maya::Element build_header() {
    // Compute market indices
    float total_change = 0;
    for (auto& s : stocks) total_change += (s.price - s.prev_close) / s.prev_close;
    total_change /= static_cast<float>(stocks.size());
    float idx_val = 5234.18f * (1.0f + total_change);

    auto spin = std::string(spinners[frame % 4]);
    std::string mkt = market_open ? "● OPEN" : "○ CLOSED";

    return (h(
        text(spin + " TERMINAL") | Bold | Fg<0, 220, 255>,
        text("TRADER") | Bold | Fg<255, 255, 255>,
        text(" ░▒▓") | Fg<0, 220, 255>,
        space,
        text("S&P") | Dim,
        text(" " + fmt_price(idx_val), gain_style(total_change)),
        text(" " + fmt_change(idx_val, 5234.18f), gain_style(total_change)),
        space,
        text(mkt, market_open ? fg(0, 220, 100) : fg(255, 60, 70)),
        text(std::string("  ") + tf_labels[timeframe]) | Bold | Fg<255, 200, 60>
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_watchlist() {
    std::vector<maya::Element> rows;

    rows.push_back((h(
        t<""> | w_<3>,
        t<"SYMBOL"> | Bold | Dim | w_<7>,
        t<"PRICE"> | Bold | Dim | w_<10>,
        t<"CHANGE"> | Bold | Dim | w_<18>,
        t<"VOL"> | Bold | Dim | w_<7>,
        t<"CHART"> | Bold | Dim
    ) | gap_<1>).build());

    for (int i = 0; i < static_cast<int>(stocks.size()); ++i) {
        auto& s = stocks[static_cast<size_t>(i)];
        float chg = s.price - s.prev_close;
        bool is_sel = (i == selected);

        std::string marker = is_sel ? " ▸ " : "   ";
        auto sel_style = is_sel ? fg(255, 255, 255).with_bold() : fg(180, 180, 190);
        auto sym_style = is_sel ? fg(0, 220, 255).with_bold() : fg(140, 180, 220);

        // Mini sparkline from last 30 points
        int n = static_cast<int>(s.history.size());
        std::vector<float> recent(s.history.end() - std::min(30, n), s.history.end());
        auto spark = spark_line(recent, 12);

        rows.push_back((h(
            text(marker, sel_style) | w_<3>,
            text(s.symbol, sym_style) | w_<7>,
            text(fmt_price(s.price), sel_style) | w_<10>,
            text(fmt_change(s.price, s.prev_close), gain_style(chg)) | w_<18>,
            text(fmt_volume(s.volume), fg(120, 120, 130)) | w_<7>,
            text(spark, chg >= 0 ? fg(0, 180, 100) : fg(220, 60, 70))
        ) | gap_<1>).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(40, 45, 55))
        .border_text(" WATCHLIST ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_chart() {
    auto& s = stocks[static_cast<size_t>(selected)];

    // Get data for selected timeframe
    int pts = tf_points[timeframe];
    int n = static_cast<int>(s.history.size());
    int start = std::max(0, n - pts);
    std::vector<float> data(s.history.begin() + start, s.history.end());

    // Braille chart
    int chart_w = 50;
    int chart_h = 8;
    auto chart_rows = braille_chart(data, chart_w, chart_h);

    float chg = s.price - s.prev_close;
    auto chart_color = chg >= 0 ? fg(0, 180, 100) : fg(220, 60, 70);

    // Y-axis labels
    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());

    std::vector<maya::Element> rows;

    // Chart header
    rows.push_back((h(
        text(s.symbol) | Bold | Fg<0, 220, 255>,
        text(" " + s.name) | Dim,
        space,
        text(fmt_price(s.price)) | Bold | Fg<255, 255, 255>,
        text(" " + fmt_change(s.price, s.prev_close), gain_style(chg))
    )).build());

    // Chart body with y-axis
    for (int r = 0; r < chart_h; ++r) {
        float label_val = mx - (mx - mn) * static_cast<float>(r) / static_cast<float>(chart_h - 1);
        char label[10];
        std::snprintf(label, sizeof(label), "%7.2f", static_cast<double>(label_val));

        rows.push_back((h(
            text(std::string(label)) | Dim | w_<8>,
            text("│") | Dim,
            text(chart_rows[static_cast<size_t>(r)], chart_color)
        )).build());
    }

    // X-axis
    std::string x_axis;
    for (int i = 0; i < chart_w; ++i) x_axis += "─";
    rows.push_back((h(
        text("") | w_<8>,
        text("└" + x_axis) | Dim
    )).build());

    // Stats row
    rows.push_back((h(
        text("Open") | Dim | w_<5>,
        text(fmt_price(s.open)) | w_<9>,
        text("High") | Dim | w_<5>,
        text(fmt_price(s.day_high)) | w_<9>,
        text("Low") | Dim | w_<5>,
        text(fmt_price(s.day_low)) | w_<9>,
        text("Vol") | Dim | w_<5>,
        text(fmt_volume(s.volume))
    ) | gap_<1>).build());

    // Volume sparkline
    int vn = static_cast<int>(s.vol_hist.size());
    int vstart = std::max(0, vn - pts);
    std::vector<float> vdata(s.vol_hist.begin() + vstart, s.vol_hist.end());
    auto vol_spark = spark_line(vdata, chart_w);

    rows.push_back((h(
        text("Vol") | Dim | w_<8>,
        text("│") | Dim,
        text(vol_spark) | Fg<60, 60, 90>
    )).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(40, 45, 55))
        .border_text(std::string(" ") + s.symbol + " · " + tf_labels[timeframe] + " ",
                     maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_news() {
    std::vector<maya::Element> rows;

    for (int i = 0; i < std::min(5, static_cast<int>(news.size())); ++i) {
        auto& n = news[static_cast<size_t>(i)];
        auto sent_icon = n.sentiment > 0 ? "▲" : (n.sentiment < 0 ? "▼" : "─");
        auto sent_col = n.sentiment > 0 ? fg(0, 200, 100) : (n.sentiment < 0 ? fg(255, 60, 70) : fg(100, 100, 110));

        rows.push_back((h(
            text(sent_icon, sent_col) | w_<2>,
            text(n.source) | Fg<100, 160, 220> | w_<10>,
            text(n.headline) | Dim,
            space,
            text(fmt_time(n.age)) | Fg<70, 70, 80>
        ) | gap_<1>).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(40, 45, 55))
        .border_text(" NEWS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_footer() {
    return (h(
        text(" ↑↓") | Bold | Fg<100, 180, 255>, text(":select") | Dim,
        text(" ←→") | Bold | Fg<100, 180, 255>, text(":timeframe") | Dim,
        text(" r") | Bold | Fg<100, 180, 255>, text(":event") | Dim,
        text(" ␣") | Bold | Fg<100, 180, 255>, text(":market") | Dim,
        text(" q") | Bold | Fg<100, 180, 255>, text(":quit") | Dim,
        space,
        text("░▒▓ maya") | Fg<40, 45, 55>
    ) | pad<0, 1, 0, 1>).build();
}

// ── Render ──────────────────────────────────────────────────────────────────

static maya::Element render() {
    tick(1.0f / 20.0f);

    return vstack()(
        build_header(),
        build_watchlist(),
        build_chart(),
        build_news(),
        build_footer()
    );
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    init();

    maya::run(
        {.title = "Terminal Trader", .fps = 20, .alt_screen = false},
        [](const maya::Event& ev) {
            using SK = maya::SpecialKey;
            maya::on(ev, 'q', [] { maya::quit(); });
            maya::on(ev, SK::Escape, [] { maya::quit(); });
            maya::on(ev, SK::Up, [] { selected = std::max(0, selected - 1); });
            maya::on(ev, 'k', [] { selected = std::max(0, selected - 1); });
            maya::on(ev, SK::Down, [] {
                selected = std::min(static_cast<int>(stocks.size()) - 1, selected + 1);
            });
            maya::on(ev, 'j', [] {
                selected = std::min(static_cast<int>(stocks.size()) - 1, selected + 1);
            });
            maya::on(ev, SK::Left, [] { timeframe = std::max(0, timeframe - 1); });
            maya::on(ev, 'h', [] { timeframe = std::max(0, timeframe - 1); });
            maya::on(ev, SK::Right, [] { timeframe = std::min(4, timeframe + 1); });
            maya::on(ev, 'l', [] { timeframe = std::min(4, timeframe + 1); });
            maya::on(ev, 'r', [] {
                // Market event: big move on random stock
                auto& s = stocks[static_cast<size_t>(randi(0, static_cast<int>(stocks.size()) - 1))];
                float shock = randf(-0.08f, 0.08f);
                s.price *= (1.0f + shock);
                s.momentum = shock > 0 ? 1.0f : -1.0f;
            });
            maya::on(ev, ' ', [] { market_open = !market_open; });
        },
        [] { return render(); }
    );

    return 0;
}
