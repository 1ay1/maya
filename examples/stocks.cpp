// stocks.cpp — Live stock ticker dashboard (inline mode)
//
// A visually rich terminal dashboard with animated charts, sparklines,
// color-coded gains/losses, portfolio summary, and a scrolling news feed.
// Uses Mode::Inline so output stays in scrollback.
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

#include <maya/app.hpp>
#include <maya/maya.hpp>

#include <chrono>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Helpers ─────────────────────────────────────────────────────────────────

namespace {

float randf(std::mt19937& rng, float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}
int randi(std::mt19937& rng, int lo, int hi) {
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
static maya::Style accent(const ColorTheme& t)   { auto c = t.accent; return fg_s(c[0], c[1], c[2]); }
static maya::Style gain_s(const ColorTheme& t)   { auto c = t.gain;   return fg_s(c[0], c[1], c[2]); }
static maya::Style loss_s(const ColorTheme& t)   { auto c = t.loss;   return fg_s(c[0], c[1], c[2]); }
static maya::Style muted(const ColorTheme& t)    { auto c = t.muted;  return fg_s(c[0], c[1], c[2]); }
static maya::Style label_s(const ColorTheme& t)  { auto c = t.label;  return fg_s(c[0], c[1], c[2]); }
static maya::Color border_c(const ColorTheme& t) { auto c = t.border; return maya::Color::rgb(c[0], c[1], c[2]); }

static maya::Style chg_style(const ColorTheme& t, float v) {
    if (v > 0) return gain_s(t).with_bold();
    if (v < 0) return loss_s(t).with_bold();
    return muted(t);
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

struct Model {
    std::mt19937          rng{std::random_device{}()};
    std::vector<Stock>    stocks;
    std::vector<NewsItem> news;
    int   selected    = 0;
    int   timeframe   = 2;
    bool  market_open = true;
    float elapsed     = 0;
    int   frame       = 0;
    int   theme_idx   = 0;
};

static const char* tf_labels[] = {"1m", "5m", "15m", "1h", "1d"};
static const int tf_points[] = {60, 120, 200, 350, 500};

static const char* spinners[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};

// ── Init ────────────────────────────────────────────────────────────────────

static void init_state(Model& m) {
    m.stocks = {
        {"AAPL",  "Apple Inc.",           189.84f, 188.50f, 187.20f, 191.30f, 187.10f,  62.4f, 0.012f, {}, {},  0.3f, 2940},
        {"NVDA",  "NVIDIA Corp.",         875.28f, 868.00f, 862.50f, 882.40f, 860.10f,  48.7f, 0.025f, {}, {},  0.8f, 2150},
        {"MSFT",  "Microsoft Corp.",      415.60f, 413.20f, 412.80f, 418.90f, 411.50f,  28.3f, 0.010f, {}, {},  0.2f, 3090},
        {"GOOGL", "Alphabet Inc.",        157.25f, 155.80f, 156.40f, 158.60f, 155.20f,  22.1f, 0.015f, {}, {}, -0.1f, 1940},
        {"AMZN",  "Amazon.com Inc.",      186.51f, 184.90f, 185.30f, 188.20f, 184.10f,  35.6f, 0.018f, {}, {},  0.5f, 1930},
        {"TSLA",  "Tesla Inc.",           248.42f, 245.00f, 243.80f, 252.30f, 242.60f,  95.2f, 0.035f, {}, {}, -0.4f,  790},
        {"META",  "Meta Platforms Inc.",  505.75f, 502.30f, 501.90f, 509.80f, 500.10f,  18.9f, 0.020f, {}, {},  0.6f, 1280},
        {"AMD",   "Advanced Micro Dev.",  164.38f, 162.50f, 161.80f, 166.70f, 161.20f,  42.8f, 0.028f, {}, {},  0.4f,  265},
    };

    for (auto& s : m.stocks) {
        s.history.resize(500);
        s.vol_hist.resize(500);
        float p = s.prev_close;
        for (int i = 0; i < 500; ++i) {
            p += randf(m.rng, -1, 1) * s.volatility * p + s.momentum * s.volatility * p * 0.1f;
            p = std::max(p, s.prev_close * 0.85f);
            s.history[static_cast<size_t>(i)] = p;
            s.vol_hist[static_cast<size_t>(i)] = randf(m.rng, 0.3f, 1.0f) * s.volume;
        }
        s.price = s.history.back();
        s.day_high = *std::max_element(s.history.begin(), s.history.end());
        s.day_low = *std::min_element(s.history.begin(), s.history.end());
    }

    m.news = {
        {"Reuters",   "Fed signals potential rate cut in September meeting",          1,   45},
        {"Bloomberg", "NVIDIA announces next-gen Blackwell Ultra GPU architecture",  1,  120},
        {"CNBC",      "Tech sector leads S&P 500 to new all-time high",              1,  230},
        {"WSJ",       "Tesla recalls 125K vehicles over seatbelt warning system",   -1,  380},
        {"Reuters",   "Apple Vision Pro sales exceed analyst expectations",           1,  510},
        {"Bloomberg", "Semiconductor supply chain bottleneck easing globally",        1,  640},
    };
}

// ── Tick ────────────────────────────────────────────────────────────────────

static void tick(Model& m, float dt) {
    m.elapsed += dt;
    m.frame++;
    if (!m.market_open) return;

    for (auto& s : m.stocks) {
        float drift = s.momentum * s.volatility * s.price * dt;
        float noise = randf(m.rng, -1, 1) * s.volatility * s.price * std::sqrt(dt) * 3.0f;
        s.price += drift + noise;
        // Cap daily move to ±20% from previous close
        s.price = std::clamp(s.price, s.prev_close * 0.80f, s.prev_close * 1.20f);
        if (randi(m.rng, 0, 200) == 0) s.momentum = randf(m.rng, -1.0f, 1.0f);

        s.history.erase(s.history.begin());
        s.history.push_back(s.price);
        s.vol_hist.erase(s.vol_hist.begin());
        s.vol_hist.push_back(randf(m.rng, 0.2f, 1.2f) * s.volume);
        s.day_high = std::max(s.day_high, s.price);
        s.day_low = std::min(s.day_low, s.price);
    }

    if (randi(m.rng, 0, 120) == 0) {
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
        int sent = randi(m.rng, 0, 2) - 1;
        m.news.insert(m.news.begin(), {
            sources[static_cast<size_t>(randi(m.rng, 0, 4))],
            m.stocks[static_cast<size_t>(randi(m.rng, 0, static_cast<int>(m.stocks.size()) - 1))].symbol + ": " +
                headlines[static_cast<size_t>(randi(m.rng, 0, 11))],
            sent, 0
        });
        if (m.news.size() > 6) m.news.pop_back();
    }
    for (auto& n : m.news) n.age += dt;
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

static maya::Element build_header(const Model& m) {
    const ColorTheme& th = themes[m.theme_idx];
    float total_change = 0;
    for (auto& s : m.stocks) total_change += (s.price - s.prev_close) / s.prev_close;
    total_change /= static_cast<float>(m.stocks.size());
    float idx_val = 5234.18f * (1.0f + total_change);

    auto spin = std::string(spinners[m.frame % 10]);
    std::string mkt = m.market_open ? "● LIVE" : "○ CLOSED";

    // Animated gradient bar
    int phase = m.frame % 24;
    std::string grad;
    const char* blocks[] = {"░","▒","▓","█","▓","▒"};
    for (int i = 0; i < 6; ++i) grad += blocks[(i + phase) % 6];

    return (h(
        text(spin, accent(th)) | w_<2>,
        text("TERMINAL", accent(th).with_bold()) | w_<9>,
        text("TRADER") | Bold | Fg<255, 255, 255>,
        text(" " + grad, accent(th)),
        space,
        text("S&P 500") | Dim | w_<8>,
        text(fmt_price(idx_val), chg_style(th, total_change)) | clip | w_<10>,
        text(fmt_change(idx_val, 5234.18f), chg_style(th, total_change)) | clip | w_<22>,
        space,
        text(mkt, m.market_open ? gain_s(th) : loss_s(th)),
        text(std::string("  ") + th.name, accent(th)) | w_<8>
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_portfolio_bar(const Model& m) {
    const ColorTheme& th = themes[m.theme_idx];
    // Portfolio summary: total value, daily P&L, top gainer/loser
    float total_val = 0, total_prev = 0;
    int best_i = 0, worst_i = 0;
    float best_pct = -999, worst_pct = 999;

    for (int i = 0; i < static_cast<int>(m.stocks.size()); ++i) {
        auto& s = m.stocks[static_cast<size_t>(i)];
        float shares = 100.0f; // pretend 100 shares each
        total_val += s.price * shares;
        total_prev += s.prev_close * shares;
        float pct = (s.price - s.prev_close) / s.prev_close;
        if (pct > best_pct) { best_pct = pct; best_i = i; }
        if (pct < worst_pct) { worst_pct = pct; worst_i = i; }
    }

    float pnl = total_val - total_prev;
    auto& best = m.stocks[static_cast<size_t>(best_i)];
    auto& worst = m.stocks[static_cast<size_t>(worst_i)];

    return (h(
        text("Portfolio") | Dim | w_<10>,
        text("$" + fmt_price(total_val)) | Bold | clip | w_<14>,
        text("P&L") | Dim | w_<4>,
        text(fmt_change(total_val, total_prev), chg_style(th, pnl)) | clip | w_<24>,
        text("│") | Dim,
        text(" ▲ " + best.symbol, gain_s(th)) | w_<9>,
        text(fmt_pct(best.price, best.prev_close), gain_s(th)) | clip | w_<12>,
        text("  ▼ " + worst.symbol, loss_s(th)) | w_<9>,
        text(fmt_pct(worst.price, worst.prev_close), loss_s(th)) | clip | w_<12>
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_watchlist(const Model& m) {
    const ColorTheme& th = themes[m.theme_idx];
    std::vector<maya::Element> rows;

    // Timeframe selector tabs
    std::vector<maya::Element> tabs;
    for (int i = 0; i < 5; ++i) {
        if (i == m.timeframe) {
            auto a = th.accent;
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
        t<"CHG"> | Bold | Dim | w_<10>,
        t<"CHG%"> | Bold | Dim | w_<10>,
        t<"MCAP"> | Bold | Dim | w_<8>,
        t<"VOL"> | Bold | Dim | w_<7>,
        t<"CHART"> | Bold | Dim
    ) | gap_<1>).build());

    for (int i = 0; i < static_cast<int>(m.stocks.size()); ++i) {
        auto& s = m.stocks[static_cast<size_t>(i)];
        float chg = s.price - s.prev_close;
        float pct = (s.prev_close > 0) ? (chg / s.prev_close * 100.0f) : 0;
        bool sel = (i == m.selected);

        std::string marker = sel ? "▸ " : "  ";
        auto sel_sty = sel ? fg_s(255, 255, 255).with_bold() : fg_s(180, 180, 190);
        auto sym_sty = sel ? accent(th).with_bold() : label_s(th);

        // Sparkline
        int n = static_cast<int>(s.history.size());
        int pts = tf_points[m.timeframe];
        std::vector<float> recent(s.history.end() - std::min(pts, n), s.history.end());
        auto spark = spark_line(recent, 16);

        char chg_buf[16]; std::snprintf(chg_buf, sizeof(chg_buf), "%+.2f", static_cast<double>(chg));
        char pct_buf[16]; std::snprintf(pct_buf, sizeof(pct_buf), "%+.2f%%", static_cast<double>(pct));

        rows.push_back((h(
            text(marker, sel_sty) | w_<2>,
            text(s.symbol, sym_sty) | w_<6>,
            text(fmt_price(s.price), sel_sty) | clip | w_<10>,
            text(std::string(chg_buf), chg_style(th, chg)) | clip | w_<10>,
            text(std::string(pct_buf), chg_style(th, chg)) | clip | w_<10>,
            text(fmt_mcap(s.market_cap), muted(th)) | clip | w_<8>,
            text(fmt_vol(s.volume), muted(th)) | w_<7>,
            text(spark, chg >= 0 ? gain_s(th) : loss_s(th))
        ) | gap_<1>).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(border_c(th))
        .border_text(" WATCHLIST ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_chart(const Model& m) {
    const ColorTheme& th = themes[m.theme_idx];
    auto& s = m.stocks[static_cast<size_t>(m.selected)];
    int pts = tf_points[m.timeframe];
    int n = static_cast<int>(s.history.size());
    std::vector<float> data(s.history.begin() + std::max(0, n - pts), s.history.end());

    int chart_w = 55;
    int chart_h = 10;
    auto chart_rows = braille_chart(data, chart_w, chart_h);

    float chg = s.price - s.prev_close;
    auto chart_col = chg >= 0 ? gain_s(th) : loss_s(th);

    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());

    std::vector<maya::Element> rows;

    // Title row
    rows.push_back((h(
        text(s.symbol, accent(th).with_bold()),
        text(" " + s.name, muted(th)) | clip,
        space,
        text("$" + fmt_price(s.price), fg_s(255, 255, 255).with_bold()),
        text(" " + fmt_change(s.price, s.prev_close), chg_style(th, chg)) | clip
    )).build());

    // Current price marker
    std::string price_tag = " $" + fmt_price(s.price) + " ";
    rows.push_back((h(
        text("") | w_<8>,
        text("┌" + price_tag, chg_style(th, chg))
    )).build());

    // Chart body with y-axis
    for (int r = 0; r < chart_h; ++r) {
        float label_val = mx - (mx - mn) * static_cast<float>(r) / static_cast<float>(chart_h - 1);
        char label[10]; std::snprintf(label, sizeof(label), "%7.2f", static_cast<double>(label_val));
        std::string sep = (r == 0) ? "┤" : "│";

        rows.push_back((h(
            text(std::string(label), muted(th)) | clip | w_<8>,
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
        text("Open", muted(th)) | w_<5>,
        text(fmt_price(s.open), fg_s(200, 200, 210)) | clip | w_<10>,
        text("High", muted(th)) | w_<5>,
        text(fmt_price(s.day_high), gain_s(th)) | clip | w_<10>,
        text("Low", muted(th)) | w_<5>,
        text(fmt_price(s.day_low), loss_s(th)) | clip | w_<10>,
        text("Vol", muted(th)) | w_<5>,
        text(fmt_vol(s.volume), fg_s(200, 200, 210)) | w_<7>,
        text("MCap", muted(th)) | w_<5>,
        text(fmt_mcap(s.market_cap), fg_s(200, 200, 210))
    ) | gap_<1>).build());

    // Volume bars
    int vn = static_cast<int>(s.vol_hist.size());
    std::vector<float> vdata(s.vol_hist.begin() + std::max(0, vn - pts), s.vol_hist.end());
    auto vol_spark = spark_line(vdata, chart_w);

    rows.push_back((h(
        text("Volume", muted(th)) | w_<8>,
        text("│") | Dim,
        text(vol_spark, muted(th))
    )).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(border_c(th))
        .border_text(std::string(" ") + s.symbol + " · " + tf_labels[m.timeframe] + " ",
                     maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_news(const Model& m) {
    const ColorTheme& th = themes[m.theme_idx];
    std::vector<maya::Element> rows;

    for (int i = 0; i < std::min(5, static_cast<int>(m.news.size())); ++i) {
        auto& n = m.news[static_cast<size_t>(i)];
        auto icon = n.sentiment > 0 ? "▲" : (n.sentiment < 0 ? "▼" : "─");
        auto col = n.sentiment > 0 ? gain_s(th) : (n.sentiment < 0 ? loss_s(th) : muted(th));

        rows.push_back((h(
            text(icon, col) | w_<2>,
            text(n.source, label_s(th)) | w_<11>,
            text(n.headline, muted(th)) | clip,
            space,
            text(fmt_time(n.age), fg_s(50, 50, 60))
        ) | gap_<1>).build());
    }

    // Fill empty slots
    while (static_cast<int>(rows.size()) < 5)
        rows.push_back(text("").build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(border_c(th))
        .border_text(" NEWS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_footer(const Model& m) {
    const ColorTheme& th = themes[m.theme_idx];
    return (h(
        text(" ↑↓", accent(th).with_bold()) | w_<4>, text("select", muted(th)) | w_<7>,
        text("←→", accent(th).with_bold()) | w_<3>, text("time", muted(th)) | w_<5>,
        text("r", accent(th).with_bold()) | w_<2>, text("event", muted(th)) | w_<6>,
        text("␣", accent(th).with_bold()) | w_<2>, text("mkt", muted(th)) | w_<4>,
        text("t", accent(th).with_bold()) | w_<2>, text("theme", muted(th)) | w_<6>,
        text("q", accent(th).with_bold()) | w_<2>, text("quit", muted(th)),
        space,
        text("powered by ", fg_s(55, 55, 70)),
        text("maya", accent(th))
    ) | pad<0, 1, 0, 1> | Bg<25, 25, 35>).build();
}

// ── Render ──────────────────────────────────────────────────────────────────

static maya::Element render(const Model& m) {
    return vstack()(
        build_header(m),
        build_portfolio_bar(m),
        build_watchlist(m),
        build_chart(m),
        build_news(m),
        build_footer(m)
    );
}

// ── Program ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Quit {};
struct Up {};
struct Down {};
struct Left {};
struct Right {};
struct Shock {};
struct ToggleMarket {};
struct CycleTheme {};
using Msg = std::variant<Tick, Quit, Up, Down, Left, Right, Shock, ToggleMarket, CycleTheme>;

struct Stocks {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key>;

    static Cmd init(Model& m) { init_state(m); return {}; }

    static Cmd update(Model& m, Tick)  { tick(m, 1.0f / 20.0f); return {}; }
    static Cmd update(Model&, Quit)    { return Cmd::quit(0); }
    static Cmd update(Model& m, Up)    { m.selected = std::max(0, m.selected - 1); return {}; }
    static Cmd update(Model& m, Down)  {
        m.selected = std::min(static_cast<int>(m.stocks.size()) - 1, m.selected + 1);
        return {};
    }
    static Cmd update(Model& m, Left)  { m.timeframe = std::max(0, m.timeframe - 1); return {}; }
    static Cmd update(Model& m, Right) { m.timeframe = std::min(4, m.timeframe + 1); return {}; }
    static Cmd update(Model& m, Shock) {
        auto& s = m.stocks[static_cast<size_t>(randi(m.rng, 0, static_cast<int>(m.stocks.size()) - 1))];
        float shock = randf(m.rng, -0.08f, 0.08f);
        s.price *= (1.0f + shock);
        s.momentum = shock > 0 ? 1.0f : -1.0f;
        return {};
    }
    static Cmd update(Model& m, ToggleMarket) { m.market_open = !m.market_open; return {}; }
    static Cmd update(Model& m, CycleTheme)   { m.theme_idx = (m.theme_idx + 1) % 4; return {}; }

    static Element view(const Model& m) { return render(m); }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            keys<Sub>({
                {'q', Quit{}},  {SpecialKey::Escape, Quit{}},
                {'k', Up{}},    {SpecialKey::Up, Up{}},
                {'j', Down{}},  {SpecialKey::Down, Down{}},
                {'h', Left{}},  {SpecialKey::Left, Left{}},
                {'l', Right{}}, {SpecialKey::Right, Right{}},
                {'r', Shock{}},
                {' ', ToggleMarket{}},
                {'t', CycleTheme{}},
            }),
            Sub::every(std::chrono::milliseconds(50), Tick{}));   // old fps = 20
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Stocks>);

}  // namespace

int main() {
    return run<Stocks>({.title = "stocks", .mode = Mode::Inline});
}
