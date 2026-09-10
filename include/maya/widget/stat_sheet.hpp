#pragma once
// maya::StatSheet — one aligned readout for a whole panel of statistics.
//
// The widget a stats surface actually needs, and the one a chart library
// does not give you: BarChart draws bars, Sparkline draws a trend, Gauge
// draws a fill — but a stats TAB is a dozen heterogeneous rows that must
// line up with each other, and alignment is the whole difference between
// a readout that reads and one that looks like debug output.
//
// ── Why a sheet and not a row widget ─────────────────────────────────────
//
// The naive decomposition is a StatRow widget the host puts in a v(). It
// cannot work: a row cannot right-align its number against the OTHER rows'
// numbers, because it does not know they exist. Every host that tried
// ended up measuring the columns itself and passing widths down — which is
// this widget, spelled once per host instead of once.
//
// So the sheet owns the measurement. It scans every row, derives one label
// column and one value column, and paints them all against those. Digits
// therefore align on their right edge (the property that lets you compare
// two numbers by eye without reading them), bars share one track, and the
// dim detail notes form a fourth column instead of ragged tails.
//
// ── What it deliberately does not know ───────────────────────────────────
//
// Numbers arrive PRE-FORMATTED. The sheet never sees a token count, a
// duration or a ratio — it sees "12.4k", "3.2s", "48%". Formatting is the
// domain's job and belongs wherever that domain's unit vocabulary lives;
// baking it in here would mean two owners of "how big is a kilotoken" and
// a widget that has to grow an enum every time a host learns a new unit.
//
// ── Usage ────────────────────────────────────────────────────────────────
//
//   StatSheet s;
//   s.hero("87%", "of routed turns ran below the Strategic model");
//   s.heading("By role");
//   s.entry({.label = "Strategic", .value = "12", .share = 0.48});
//   s.entry({.label = "Utility",   .value = "13", .share = 0.52});
//   s.heading("Throughput");
//   s.entry({.label = "Output", .value = "1.2k/s", .spark = rate_history});
//   auto ui = s.build();

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../text/unicode_width.hpp"

namespace maya {

struct StatSheetTheme {
    Color label   = Color::bright_white();   // the row's name
    Color value   = Color::bright_white();   // the number itself
    Color detail  = Color::bright_black();   // trailing note / share
    Color heading = Color::cyan();           // section title
    Color bar     = Color::cyan();           // filled portion of a track
    Color track   = Color::bright_black();   // the unfilled remainder
    Color hero    = Color::cyan();           // the headline figure
};

// Eighth-block ramp. A share of 3% in a 12-cell track is a third of one
// cell, and a bar that floors to empty says "this model did no work at
// all" — a different and wrong claim. The partial glyph keeps the
// distinction between "almost none" and "none" visible, which is exactly
// the distinction a stats panel exists to show.
namespace stat_detail {

inline constexpr std::string_view kEighths[8] = {
    "\xe2\x96\x8f",  // ▏ 1/8
    "\xe2\x96\x8e",  // ▎ 2/8
    "\xe2\x96\x8d",  // ▍ 3/8
    "\xe2\x96\x8c",  // ▌ 4/8
    "\xe2\x96\x8b",  // ▋ 5/8
    "\xe2\x96\x8a",  // ▊ 6/8
    "\xe2\x96\x89",  // ▉ 7/8
    "\xe2\x96\x88",  // █ 8/8
};

// ▁▂▃▄▅▆▇█ — one column per sample, height by magnitude.
inline constexpr std::string_view kSpark[8] = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
};

// Filled prefix of a `cells`-wide track, in eighths.
[[nodiscard]] inline std::string bar_fill(double share, int cells) {
    if (cells <= 0) return {};
    if (share < 0.0) share = 0.0;
    if (share > 1.0) share = 1.0;
    const int eighths = static_cast<int>(share * cells * 8.0 + 0.5);
    std::string out;
    int full = eighths / 8;
    const int rem = eighths % 8;
    // A non-zero share must never render as nothing. Rounding is allowed
    // to make a bar SHORT, never to make it absent.
    if (full == 0 && rem == 0 && share > 0.0) {
        out += kEighths[0];
        return out;
    }
    if (full > cells) full = cells;
    for (int i = 0; i < full; ++i) out += kEighths[7];
    if (rem > 0 && full < cells) out += kEighths[static_cast<std::size_t>(rem - 1)];
    return out;
}

// How many display columns bar_fill() just produced.
[[nodiscard]] inline int bar_cells(double share, int cells) {
    if (cells <= 0) return 0;
    if (share < 0.0) share = 0.0;
    if (share > 1.0) share = 1.0;
    const int eighths = static_cast<int>(share * cells * 8.0 + 0.5);
    int full = eighths / 8;
    const int rem = eighths % 8;
    if (full == 0 && rem == 0 && share > 0.0) return 1;
    if (full > cells) full = cells;
    return full + (rem > 0 && full < cells ? 1 : 0);
}

[[nodiscard]] inline std::string spark_of(const std::vector<double>& xs,
                                          int cells) {
    if (xs.empty() || cells <= 0) return {};
    // Show the most RECENT `cells` samples. A trend strip that shows the
    // oldest window is answering a question nobody asked.
    const std::size_t take =
        xs.size() > static_cast<std::size_t>(cells)
            ? static_cast<std::size_t>(cells) : xs.size();
    const std::size_t from = xs.size() - take;
    double hi = 0.0;
    for (std::size_t i = from; i < xs.size(); ++i)
        if (xs[i] > hi) hi = xs[i];
    std::string out;
    for (std::size_t i = from; i < xs.size(); ++i) {
        // Scale against the window's own peak: a sparkline is about SHAPE,
        // and a fixed ceiling flattens every real series into a flat line.
        const double f = hi > 0.0 ? xs[i] / hi : 0.0;
        int idx = static_cast<int>(f * 7.0 + 0.5);
        if (idx < 0) idx = 0;
        if (idx > 7) idx = 7;
        out += kSpark[static_cast<std::size_t>(idx)];
    }
    return out;
}

}  // namespace stat_detail

// One measured row. Everything is optional except the label, and what is
// absent simply does not draw — so the same type serves a plain key/value,
// a ranked bar, a trend strip and a full-width meter without four types
// and four renderers.
struct StatEntry {
    std::string label;
    // Pre-formatted. See the header note: the sheet does not do units.
    std::string value;
    // Dim trailing note — a share, a count, a "p95" qualifier. Its own
    // column, so notes line up instead of trailing raggedly off values.
    std::string detail;
    // < 0 draws no bar. 0 draws an empty track, which is a real reading
    // ("this bucket exists and is empty") and not the same as no bar.
    double share = -1.0;
    // Non-empty replaces the bar with a trend strip over these samples.
    std::vector<double> spark;
    // Per-row hue for the bar / value. Absent = the theme's colour. This
    // is how a Tools tab colours failures red without the sheet knowing
    // what a tool is.
    std::optional<Color> hue;
    // Let the bar take every remaining column instead of the shared track
    // — the "meter" shape, for a context-window fill where the row IS the
    // chart rather than one of a ranked set.
    bool wide = false;
};

struct StatHeading { std::string text; };
struct StatBlank   {};
// The headline: one figure and the sentence it answers. A stats tab that
// opens with a table makes the reader derive the answer; a tab that opens
// with the answer and then shows its working does not.
struct StatHero    { std::string value; std::string caption;
                     std::optional<Color> hue; };

class StatSheet {
public:
    using Row = std::variant<StatHeading, StatBlank, StatHero, StatEntry>;

    StatSheetTheme theme{};

    StatSheet& heading(std::string t) {
        rows_.emplace_back(StatHeading{std::move(t)});
        return *this;
    }
    StatSheet& blank() { rows_.emplace_back(StatBlank{}); return *this; }
    StatSheet& hero(std::string value, std::string caption,
                    std::optional<Color> hue = std::nullopt) {
        rows_.emplace_back(StatHero{std::move(value), std::move(caption), hue});
        return *this;
    }
    StatSheet& entry(StatEntry e) {
        rows_.emplace_back(std::move(e));
        return *this;
    }

    // Track width for ranked bars, in columns. The default reads at the
    // panel widths this ships at; a narrow surface shrinks it automatically
    // (see the degradation ladder in build()).
    StatSheet& track(int cells) { track_ = cells; return *this; }
    // Left inset, matching the panel's own gutter.
    StatSheet& indent(int cols) { indent_ = cols; return *this; }

    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return detail::component([rows = rows_, theme = theme,
                                  track = track_, indent = indent_]
                                 (int avail_w, int) -> Element {
            const int avail = avail_w > indent ? avail_w - indent : 0;

            // ── Measure once, for the whole sheet ────────────────────────
            //
            // This is the widget's reason to exist. Every column below is a
            // max over ALL entries, so a row is painted against the sheet's
            // geometry rather than its own.
            int label_w = 0, value_w = 0, detail_w = 0;
            bool any_bar = false;
            for (const auto& r : rows) {
                const auto* e = std::get_if<StatEntry>(&r);
                if (!e) continue;
                label_w  = std::max(label_w,  unicode::str_width(e->label));
                value_w  = std::max(value_w,  unicode::str_width(e->value));
                detail_w = std::max(detail_w, unicode::str_width(e->detail));
                if (e->share >= 0.0 || !e->spark.empty()) any_bar = true;
            }

            // ── Degrade in the order that loses the least ────────────────
            //
            // A stats row is label + chart + number + note, and they are not
            // equally load-bearing. The NUMBER is the datum, so it is last
            // to go; the note is redundant with it, so it goes first; the
            // chart is a comparison aid that only pays for itself with room
            // to be proportional, so it goes second and shrinks before it
            // vanishes. The label is truncated rather than dropped, because
            // a number whose subject is unknown is not a statistic.
            int bar_w = any_bar ? track : 0;
            const int kGap = 2;
            auto total = [&] {
                int t = label_w + value_w;
                if (bar_w)    t += bar_w + kGap;
                if (detail_w) t += detail_w + kGap;
                return t + kGap;
            };
            if (total() > avail && detail_w) detail_w = 0;
            while (total() > avail && bar_w > 4) --bar_w;
            if (total() > avail && bar_w) bar_w = 0;
            if (total() > avail) {
                const int slack = total() - avail;
                label_w = std::max(4, label_w - slack);
            }

            std::vector<Element> out;
            out.reserve(rows.size());
            const std::string pad(static_cast<std::size_t>(indent), ' ');

            for (const auto& r : rows) {
                std::string s;
                std::vector<StyledRun> runs;
                auto put = [&](std::string_view t, Style st) {
                    if (t.empty()) return;
                    runs.push_back(StyledRun{s.size(), t.size(), st});
                    s += t;
                };
                auto gap = [&](int n) {
                    if (n > 0) s.append(static_cast<std::size_t>(n), ' ');
                };
                s += pad;

                if (const auto* h = std::get_if<StatHeading>(&r)) {
                    put(h->text, Style{}.with_fg(theme.heading).with_bold());
                } else if (std::holds_alternative<StatBlank>(r)) {
                    // A blank row is a blank row; no styling, no runs.
                } else if (const auto* hero = std::get_if<StatHero>(&r)) {
                    put(hero->value,
                        Style{}.with_fg(hero->hue.value_or(theme.hero)).with_bold());
                    if (!hero->caption.empty()) {
                        gap(1);
                        put(hero->caption, Style{}.with_fg(theme.label));
                    }
                } else {
                    const auto& e = std::get<StatEntry>(r);
                    const Color hue = e.hue.value_or(theme.bar);

                    // Label, padded to the shared column.
                    std::string lab = e.label;
                    int lw = unicode::str_width(lab);
                    if (lw > label_w) {
                        // Truncate by DISPLAY COLUMNS, never by bytes — a
                        // multi-byte label cut mid-sequence renders as a
                        // replacement glyph and takes the row's alignment
                        // with it.
                        lab = std::string{
                            unicode::truncate_to_width(lab, label_w)};
                        lw = unicode::str_width(lab);
                    }
                    put(lab, Style{}.with_fg(theme.label));
                    gap(label_w - lw);
                    gap(kGap);

                    // Chart column: a trend strip, a proportional bar, or
                    // nothing — all occupying the same columns, so a sheet
                    // that mixes them still aligns.
                    //
                    // A `wide` row is the exception on purpose: it is a
                    // METER, the row IS the chart, so it takes every column
                    // its own value does not need and ends flush at the
                    // right edge. Reserving the shared detail column for a
                    // row that has no detail is what left it ragged.
                    const int wide_room =
                        avail - label_w - value_w - kGap * 2
                              - (e.detail.empty() || !detail_w
                                     ? 0 : detail_w + kGap);
                    const int this_bar =
                        e.wide && bar_w ? std::max(bar_w, wide_room) : bar_w;
                    if (this_bar > 0 && !e.spark.empty()) {
                        const auto sp = stat_detail::spark_of(e.spark, this_bar);
                        put(sp, Style{}.with_fg(hue));
                        gap(this_bar - unicode::str_width(sp));
                        gap(kGap);
                    } else if (this_bar > 0 && e.share >= 0.0) {
                        const auto fill = stat_detail::bar_fill(e.share, this_bar);
                        const int used = stat_detail::bar_cells(e.share, this_bar);
                        put(fill, Style{}.with_fg(hue));
                        // The unfilled remainder is DRAWN, not left blank.
                        // An empty tail makes a short bar read as a missing
                        // bar; a visible track says "this is the scale, and
                        // you are here on it".
                        std::string rest;
                        for (int i = used; i < this_bar; ++i)
                            rest += "\xe2\x94\x80";   // ─
                        put(rest, Style{}.with_fg(theme.track));
                        gap(kGap);
                    } else if (bar_w > 0) {
                        gap(bar_w + kGap);
                    }

                    // Value, RIGHT-aligned in the shared column. Digits that
                    // line up on their right edge can be compared without
                    // being read; left-aligned numbers cannot.
                    const int vw = unicode::str_width(e.value);
                    gap(value_w - vw);
                    put(e.value,
                        Style{}.with_fg(e.hue.value_or(theme.value)).with_bold());

                    if (detail_w > 0 && !e.detail.empty()) {
                        gap(kGap);
                        const int dw = unicode::str_width(e.detail);
                        gap(detail_w - dw);
                        put(e.detail, Style{}.with_fg(theme.detail));
                    }
                }

                out.push_back(Element{TextElement{
                    .content = std::move(s),
                    .wrap    = TextWrap::TruncateEnd,
                    .runs    = std::move(runs),
                }});
            }

            return dsl::v(std::move(out)).build();
        }).build();
    }

private:
    std::vector<Row> rows_;
    int track_  = 14;
    int indent_ = 0;
};

}  // namespace maya
