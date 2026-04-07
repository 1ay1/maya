// stocks.cpp — Live stock ticker dashboard (inline mode)
//
// A visually rich terminal dashboard with animated charts, sparklines,
// color-coded gains/losses, portfolio summary, and a scrolling news feed.
// Uses maya::run with alt_screen=false so output stays in scrollback.
//
// All data is simulated with correlated random walks.
//
// Controls:
//   ↑/↓  k/j    select stock
//   ←/→  h/l    change timeframe (1m 5m 15m 1h 1d)
//   r           trigger random market event
//   space       toggle market open/closed
//   t           cycle color theme
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
#include <numeric>
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

static maya::Style fg_s(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}
static maya::Style bg_s(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_bg(maya::Color::rgb(r, g, b));
}

// ── Theme ───────────────────────────────────────────────────────────────────

struct ColorTheme {
    const char* name;
    // accent, gain, loss, muted, border, header_bg, label
    uint8_t accent[3], gain[3], loss[3], muted[3], border[3], dim[3], label[3];
};

static const ColorTheme themes[] = {
    {"NEON",
     {0, 220, 255},   {0, 255, 120},   {255, 50, 80},   {80, 80, 100},
     {35, 40, 55},    {60, 60, 75},     {140, 180, 220}},
    {"AMBER",
     {255, 180, 0},   {80, 255, 120},   {255, 80, 60},   {120, 100, 60},
     {50, 42, 25},    {80, 70, 45},     {200, 170, 100}},
    {"VAPOR",
     {255, 100, 220}, {100, 255, 200},  {255, 80, 100},  {100, 70, 120},
     {45, 25, 55},    {70, 40, 80},     {180, 140, 220}},
    {"MATRIX",
     {0, 255, 65},    {0, 255, 65},     {255, 50, 50},   {0, 100, 30},
     {0, 40, 15},     {0, 60, 20},      {0, 180, 60}},
};
static int theme_idx = 0;

static const ColorTheme& thm() { return themes[theme_idx]; }
static maya::Style accent()    { auto c = thm().accent; return fg_s(c[0], c[1], c[2]); }
static maya::Style gain_s()    { auto c = thm().gain;   return fg_s(c[0], c[1], c[2]); }
static maya::Style loss_s()    { auto c = thm().loss;   return fg_s(c[0], c[1], c[2]); }
static maya::Style muted()     { auto c = thm().muted;  return fg_s(c[0], c[1], c[2]); }
static maya::Style label_s()   { auto c = thm().label;  return fg_s(c[0], c[1], c[2]); }
static maya::Color border_c()  { auto c = thm().border; return maya::Color::rgb(c[0], c[1], c[2]); }

static maya::Style chg_style(float v) {
    if (v > 0) return gain_s().with_bold();
    if (v < 0) return loss_s().with_bold();
    return muted();
}

// ── Spark / Chart rendering ─────────────────────────────────────────────────

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

// Braille chart (2x4 dot matrix per cell)
static std::vector<std::string> braille_chart(const std::vector<float>& data, int width, int height) {
    if (data.empty()) return std::vector<std::string>(static_cast<size_t>(height), "");

    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());
    float range = mx - mn;
    if (range < 0.001f) { mn -= 1; mx += 1; range = 2; }

    int dot_rows = height * 4;
    int dot_cols = width * 2;

    std::vector<int> dot_y(static_cast<size_t>(dot_cols), 0);
    for (int dx = 0; dx < dot_cols; ++dx) {
        int di = std::clamp(
            static_cast<int>(static_cast<float>(dx) / static_cast<float>(dot_cols) * static_cast<float>(data.size())),
            0, static_cast<int>(data.size()) - 1);
        float v = data[static_cast<size_t>(di)];
        dot_y[static_cast<size_t>(dx)] = std::clamp(
            static_cast<int>((v - mn) / range * static_cast<float>(dot_rows - 1)),
            0, dot_rows - 1);
    }

    static constexpr uint8_t dot_bits[2][4] = {
        {0x40, 0x04, 0x02, 0x01},
        {0x80, 0x20, 0x10, 0x08},
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
                int cell_top_row = (height - 1 - cy) * 4;
                for (int dr = 0; dr < 4; ++dr) {
                    int abs_row = cell_top_row + (3 - dr);
                    if (abs_row <= dy && abs_row >= dy - 1) bits |= dot_bits[dc][dr];
                }
            }
            char32_t cp = 0x2800 + bits;
            row += static_cast<char>(0xE0 | (cp >> 12));
            row += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            row += static_cast<char>(0x80 | (cp & 0x3F));
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
    float volume;       // millions
    float volatility;
    std::vector<float> history;
    std::vector<float> vol_hist;
    float momentum = 0;
    float market_cap;   // billions
};

struct NewsItem {
    std::string source;
    std::string headline;
    int sentiment;
    float age;
};

// ── State ───────────────────────────────────────────────────────────────────

static std::vector<Stock> stocks;
static std::vector<NewsItem> news;
static int selected = 0;
static int timeframe = 2;
static const char* tf_labels[] = {"1m", "5m", "15m", "1h", "1d"};
static const int tf_points[] = {60, 120, 200, 350, 500};
static bool market_open = true;
static float elapsed = 0;
static int frame = 0;

static const char* spinners[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};

// ── Init ────────────────────────────────────────────────────────────────────

static void init() {
    stocks = {
        {"AAPL",  "Apple Inc.",           189.84f, 188.50f, 187.20f, 191.30f, 187.10f,  62.4f, 0.012f, {}, {},  0.3f, 2940},
        {"NVDA",  "NVIDIA Corp.",         875.28f, 868.00f, 862.50f, 882.40f, 860.10f,  48.7f, 0.025f, {}, {},  0.8f, 2150},
        {"MSFT",  "Microsoft Corp.",      415.60f, 413.20f, 412.80f, 418.90f, 411.50f,  28.3f, 0.010f, {}, {},  0.2f, 3090},
        {"GOOGL", "Alphabet Inc.",        157.25f, 155.80f, 156.40f, 158.60f, 155.20f,  22.1f, 0.015f, {}, {}, -0.1f, 1940},
        {"AMZN",  "Amazon.com Inc.",      186.51f, 184.90f, 185.30f, 188.20f, 184.10f,  35.6f, 0.018f, {}, {},  0.5f, 1930},
        {"TSLA",  "Tesla Inc.",           248.42f, 245.00f, 243.80f, 252.30f, 242.60f,  95.2f, 0.035f, {}, {}, -0.4f,  790},
        {"META",  "Meta Platforms Inc.",  505.75f, 502.30f, 501.90f, 509.80f, 500.10f,  18.9f, 0.020f, {}, {},  0.6f, 1280},
        {"AMD",   "Advanced Micro Dev.",  164.38f, 162.50f, 161.80f, 166.70f, 161.20f,  42.8f, 0.028f, {}, {},  0.4f,  265},
    };

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
        {"Reuters",   "Fed signals potential rate cut in September meeting",          1,   45},
        {"Bloomberg", "NVIDIA announces next-gen Blackwell Ultra GPU architecture",  1,  120},
        {"CNBC",      "Tech sector leads S&P 500 to new all-time high",              1,  230},
        {"WSJ",       "Tesla recalls 125K vehicles over seatbelt warning system",   -1,  380},
        {"Reuters",   "Apple Vision Pro sales exceed analyst expectations",           1,  510},
        {"Bloomberg", "Semiconductor supply chain bottleneck easing globally",        1,  640},
    };
}

// ── Tick ────────────────────────────────────────────────────────────────────

static void tick(float dt) {
    elapsed += dt;
    frame++;
    if (!market_open) return;

    for (auto& s : stocks) {
        float drift = s.momentum * s.volatility * s.price * dt;
        float noise = randf(-1, 1) * s.volatility * s.price * std::sqrt(dt) * 3.0f;
        s.price += drift + noise;
        s.price = std::max(s.price, s.prev_close * 0.80f);
        if (randi(0, 200) == 0) s.momentum = randf(-1.0f, 1.0f);

        s.history.erase(s.history.begin());
        s.history.push_back(s.price);
        s.vol_hist.erase(s.vol_hist.begin());
        s.vol_hist.push_back(randf(0.2f, 1.2f) * s.volume);
        s.day_high = std::max(s.day_high, s.price);
        s.day_low = std::min(s.day_low, s.price);
    }

    if (randi(0, 120) == 0) {
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
        static const std::array<std::string, 5> sources = {"Reuters","Bloomberg","CNBC","WSJ","FT"};
        int sent = randi(0, 2) - 1;
        news.insert(news.begin(), {
            sources[static_cast<size_t>(randi(0, 4))],
            stocks[static_cast<size_t>(randi(0, static_cast<int>(stocks.size()) - 1))].symbol + ": " +
                headlines[static_cast<size_t>(randi(0, 11))],
            sent, 0
        });
        if (news.size() > 6) news.pop_back();
    }
    for (auto& n : news) n.age += dt;
}

// ── Format helpers ──────────────────────────────────────────────────────────

static std::string fmt_price(float p) {
    char buf[16]; std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(p)); return buf;
}
static std::string fmt_change(float price, float ref) {
    float diff = price - ref;
    float pct = (ref > 0) ? (diff / ref * 100.0f) : 0.0f;
    char buf[32]; std::snprintf(buf, sizeof(buf), "%+.2f (%+.2f%%)", static_cast<double>(diff), static_cast<double>(pct)); return buf;
}
static std::string fmt_pct(float price, float ref) {
    float pct = (ref > 0) ? ((price - ref) / ref * 100.0f) : 0.0f;
    char buf[16]; std::snprintf(buf, sizeof(buf), "%+.2f%%", static_cast<double>(pct)); return buf;
}
static std::string fmt_vol(float v) {
    if (v >= 1000) { char b[16]; std::snprintf(b, sizeof(b), "%.1fB", static_cast<double>(v / 1000)); return b; }
    if (v >= 1) { char b[16]; std::snprintf(b, sizeof(b), "%.1fM", static_cast<double>(v)); return b; }
    char b[16]; std::snprintf(b, sizeof(b), "%.0fK", static_cast<double>(v * 1000)); return b;
}
static std::string fmt_mcap(float b) {
    char buf[16]; std::snprintf(buf, sizeof(buf), "$%.1fT", static_cast<double>(b / 1000)); return buf;
}
static std::string fmt_time(float secs) {
    if (secs < 60) return std::to_string(static_cast<int>(secs)) + "s";
    if (secs < 3600) return std::to_string(static_cast<int>(secs / 60)) + "m";
    return std::to_string(static_cast<int>(secs / 3600)) + "h";
}

// ── UI Builders ─────────────────────────────────────────────────────────────

static maya::Element build_header() {
    float total_change = 0;
    for (auto& s : stocks) total_change += (s.price - s.prev_close) / s.prev_close;
    total_change /= static_cast<float>(stocks.size());
    float idx_val = 5234.18f * (1.0f + total_change);

    auto spin = std::string(spinners[frame % 10]);
    std::string mkt = market_open ? "● LIVE" : "○ CLOSED";

    // Animated gradient bar
    int phase = frame % 24;
    std::string grad;
    const char* blocks[] = {"░","▒","▓","█","▓","▒"};
    for (int i = 0; i < 6; ++i) grad += blocks[(i + phase) % 6];

    return (h(
        text(spin, accent()) | w_<2>,
        text("TERMINAL", accent().with_bold()) | w_<9>,
        text("TRADER") | Bold | Fg<255, 255, 255>,
        text(" " + grad, accent()),
        space,
        text("S&P 500") | Dim | w_<8>,
        text(fmt_price(idx_val), chg_style(total_change)) | w_<9>,
        text(fmt_change(idx_val, 5234.18f), chg_style(total_change)) | w_<18>,
        space,
        text(mkt, market_open ? gain_s() : loss_s()),
        text(std::string("  ") + thm().name, accent()) | w_<8>
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_portfolio_bar() {
    // Portfolio summary: total value, daily P&L, top gainer/loser
    float total_val = 0, total_prev = 0;
    int best_i = 0, worst_i = 0;
    float best_pct = -999, worst_pct = 999;

    for (int i = 0; i < static_cast<int>(stocks.size()); ++i) {
        auto& s = stocks[static_cast<size_t>(i)];
        float shares = 100.0f; // pretend 100 shares each
        total_val += s.price * shares;
        total_prev += s.prev_close * shares;
        float pct = (s.price - s.prev_close) / s.prev_close;
        if (pct > best_pct) { best_pct = pct; best_i = i; }
        if (pct < worst_pct) { worst_pct = pct; worst_i = i; }
    }

    float pnl = total_val - total_prev;
    auto& best = stocks[static_cast<size_t>(best_i)];
    auto& worst = stocks[static_cast<size_t>(worst_i)];

    return (h(
        text("Portfolio") | Dim | w_<10>,
        text("$" + fmt_price(total_val)) | Bold | w_<12>,
        text("P&L") | Dim | w_<4>,
        text(fmt_change(total_val, total_prev), chg_style(pnl)) | w_<20>,
        text("│") | Dim,
        text(" ▲ " + best.symbol, gain_s()) | w_<9>,
        text(fmt_pct(best.price, best.prev_close), gain_s()),
        text("  ▼ " + worst.symbol, loss_s()) | w_<9>,
        text(fmt_pct(worst.price, worst.prev_close), loss_s())
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_watchlist() {
    std::vector<maya::Element> rows;

    // Timeframe selector tabs
    std::vector<maya::Element> tabs;
    for (int i = 0; i < 5; ++i) {
        if (i == timeframe) {
            auto a = thm().accent;
            tabs.push_back(text(std::string(" ") + tf_labels[i] + " ",
                               bg_s(a[0], a[1], a[2]).with_fg(maya::Color::rgb(0, 0, 0)).with_bold()).build());
        } else {
            tabs.push_back((text(std::string(" ") + tf_labels[i] + " ") | Dim).build());
        }
    }
    rows.push_back(hstack().gap(1)(std::move(tabs)));

    // Column headers
    rows.push_back((h(
        t<""> | w_<2>,
        t<"SYMBOL"> | Bold | Dim | w_<6>,
        t<"LAST"> | Bold | Dim | w_<10>,
        t<"CHG"> | Bold | Dim | w_<8>,
        t<"CHG%"> | Bold | Dim | w_<8>,
        t<"MCAP"> | Bold | Dim | w_<8>,
        t<"VOL"> | Bold | Dim | w_<7>,
        t<"CHART"> | Bold | Dim
    ) | gap_<1>).build());

    for (int i = 0; i < static_cast<int>(stocks.size()); ++i) {
        auto& s = stocks[static_cast<size_t>(i)];
        float chg = s.price - s.prev_close;
        float pct = (s.prev_close > 0) ? (chg / s.prev_close * 100.0f) : 0;
        bool sel = (i == selected);

        std::string marker = sel ? "▸ " : "  ";
        auto sel_sty = sel ? fg_s(255, 255, 255).with_bold() : fg_s(180, 180, 190);
        auto sym_sty = sel ? accent().with_bold() : label_s();

        // Sparkline
        int n = static_cast<int>(s.history.size());
        int pts = tf_points[timeframe];
        std::vector<float> recent(s.history.end() - std::min(pts, n), s.history.end());
        auto spark = spark_line(recent, 16);

        char chg_buf[16]; std::snprintf(chg_buf, sizeof(chg_buf), "%+.2f", static_cast<double>(chg));
        char pct_buf[16]; std::snprintf(pct_buf, sizeof(pct_buf), "%+.2f%%", static_cast<double>(pct));

        rows.push_back((h(
            text(marker, sel_sty) | w_<2>,
            text(s.symbol, sym_sty) | w_<6>,
            text(fmt_price(s.price), sel_sty) | w_<10>,
            text(std::string(chg_buf), chg_style(chg)) | w_<8>,
            text(std::string(pct_buf), chg_style(chg)) | w_<8>,
            text(fmt_mcap(s.market_cap), muted()) | w_<8>,
            text(fmt_vol(s.volume), muted()) | w_<7>,
            text(spark, chg >= 0 ? gain_s() : loss_s())
        ) | gap_<1>).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(border_c())
        .border_text(" WATCHLIST ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_chart() {
    auto& s = stocks[static_cast<size_t>(selected)];
    int pts = tf_points[timeframe];
    int n = static_cast<int>(s.history.size());
    std::vector<float> data(s.history.begin() + std::max(0, n - pts), s.history.end());

    int chart_w = 55;
    int chart_h = 10;
    auto chart_rows = braille_chart(data, chart_w, chart_h);

    float chg = s.price - s.prev_close;
    auto chart_col = chg >= 0 ? gain_s() : loss_s();

    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());

    std::vector<maya::Element> rows;

    // Title row
    rows.push_back((h(
        text(s.symbol, accent().with_bold()),
        text(" " + s.name, muted()),
        space,
        text("$" + fmt_price(s.price), fg_s(255, 255, 255).with_bold()),
        text(" " + fmt_change(s.price, s.prev_close), chg_style(chg))
    )).build());

    // Current price marker
    std::string price_tag = " $" + fmt_price(s.price) + " ";
    rows.push_back((h(
        text("") | w_<8>,
        text("┌" + price_tag, chg_style(chg))
    )).build());

    // Chart body with y-axis
    for (int r = 0; r < chart_h; ++r) {
        float label_val = mx - (mx - mn) * static_cast<float>(r) / static_cast<float>(chart_h - 1);
        char label[10]; std::snprintf(label, sizeof(label), "%7.2f", static_cast<double>(label_val));
        std::string sep = (r == 0) ? "┤" : "│";

        rows.push_back((h(
            text(std::string(label), muted()) | w_<8>,
            text(sep) | Dim,
            text(chart_rows[static_cast<size_t>(r)], chart_col)
        )).build());
    }

    // X-axis
    std::string x_axis;
    for (int i = 0; i < chart_w; ++i) x_axis += "─";
    rows.push_back((h(
        text("") | w_<8>,
        text("└" + x_axis) | Dim
    )).build());

    // Stats cards
    rows.push_back((h(
        text("Open", muted()) | w_<5>,
        text(fmt_price(s.open), fg_s(200, 200, 210)) | w_<9>,
        text("High", muted()) | w_<5>,
        text(fmt_price(s.day_high), gain_s()) | w_<9>,
        text("Low", muted()) | w_<5>,
        text(fmt_price(s.day_low), loss_s()) | w_<9>,
        text("Vol", muted()) | w_<5>,
        text(fmt_vol(s.volume), fg_s(200, 200, 210)) | w_<7>,
        text("MCap", muted()) | w_<5>,
        text(fmt_mcap(s.market_cap), fg_s(200, 200, 210))
    ) | gap_<1>).build());

    // Volume bars
    int vn = static_cast<int>(s.vol_hist.size());
    std::vector<float> vdata(s.vol_hist.begin() + std::max(0, vn - pts), s.vol_hist.end());
    auto vol_spark = spark_line(vdata, chart_w);

    rows.push_back((h(
        text("Volume", muted()) | w_<8>,
        text("│") | Dim,
        text(vol_spark, muted())
    )).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(border_c())
        .border_text(std::string(" ") + s.symbol + " · " + tf_labels[timeframe] + " ",
                     maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_news() {
    std::vector<maya::Element> rows;

    for (int i = 0; i < std::min(5, static_cast<int>(news.size())); ++i) {
        auto& n = news[static_cast<size_t>(i)];
        auto icon = n.sentiment > 0 ? "▲" : (n.sentiment < 0 ? "▼" : "─");
        auto col = n.sentiment > 0 ? gain_s() : (n.sentiment < 0 ? loss_s() : muted());

        rows.push_back((h(
            text(icon, col) | w_<2>,
            text(n.source, label_s()) | w_<11>,
            text(n.headline, muted()),
            space,
            text(fmt_time(n.age), fg_s(50, 50, 60))
        ) | gap_<1>).build());
    }

    // Fill empty slots
    while (static_cast<int>(rows.size()) < 5)
        rows.push_back(text("").build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(border_c())
        .border_text(" NEWS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_footer() {
    return (h(
        text(" ↑↓", accent().with_bold()) | w_<4>, text("select", muted()) | w_<7>,
        text("←→", accent().with_bold()) | w_<3>, text("time", muted()) | w_<5>,
        text("r", accent().with_bold()) | w_<2>, text("event", muted()) | w_<6>,
        text("␣", accent().with_bold()) | w_<2>, text("mkt", muted()) | w_<4>,
        text("t", accent().with_bold()) | w_<2>, text("theme", muted()) | w_<6>,
        text("q", accent().with_bold()) | w_<2>, text("quit", muted()),
        space,
        text("powered by ", fg_s(35, 35, 45)),
        text("maya", accent())
    ) | pad<0, 1, 0, 1>).build();
}

// ── Render ──────────────────────────────────────────────────────────────────

static maya::Element render() {
    tick(1.0f / 20.0f);

    return vstack()(
        build_header(),
        build_portfolio_bar(),
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
            maya::on(ev, SK::Right, [] { timeframe = std::min(4, timeframe + 1); });
            maya::on(ev, 'r', [] {
                auto& s = stocks[static_cast<size_t>(randi(0, static_cast<int>(stocks.size()) - 1))];
                float shock = randf(-0.08f, 0.08f);
                s.price *= (1.0f + shock);
                s.momentum = shock > 0 ? 1.0f : -1.0f;
            });
            maya::on(ev, ' ', [] { market_open = !market_open; });
            maya::on(ev, 't', [] { theme_idx = (theme_idx + 1) % 4; });
        },
        [] { return render(); }
    );

    return 0;
}
