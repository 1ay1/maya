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
//
// These are FILLED by construction: a lower block is solid from the
// baseline up, so a run of them reads as an area chart rather than as a
// dotted outline. That is why the inline trend uses blocks and the
// multi-row figure has to fill explicitly — see fill_below.
inline constexpr std::string_view kSpark[8] = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
};

// The same ramp under its structural name: eighths growing UPWARD from the
// baseline, as against kEighths which grows rightward from the left edge.
// One is how an area column is built, the other how a bar is; they happen
// to be the same eight code points rotated, and conflating them is how a
// bar ends up drawn with ▁.
inline constexpr const std::string_view* kEighthsUp = kSpark;

// One braille cell is a 2×4 dot matrix, so a plot drawn in braille has
// EIGHT times the resolution of one drawn in blocks. That is the whole
// reason a stats panel can show a real curve in four terminal rows.
//
//   col0 col1
//   0x01 0x08   row 0
//   0x02 0x10   row 1
//   0x04 0x20   row 2
//   0x40 0x80   row 3
inline constexpr std::uint8_t kBrailleDot[4][2] = {
    {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80},
};

inline constexpr std::string_view kFullBlock = "\xe2\x96\x88";   // █
inline constexpr std::string_view kLegendDot = "\xe2\x96\xa0";   // ■

// U+2800 + bits, as UTF-8. A blank cell is U+2800 itself (all dots
// clear), NOT a space: a space would collapse under the trailing-space
// trim every row does and the plot would lose its right edge.
[[nodiscard]] inline std::string braille(std::uint8_t bits) {
    const unsigned cp = 0x2800u + bits;
    std::string s;
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
    return s;
}

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

// A COMPOSITION bar: one full-width track split into coloured segments
// that sum to the whole, with a legend beneath.
//
// Different question from a ranked list of bars, which is why it is a
// different row. Ranked bars answer "how big is each one" — you compare
// lengths. A band answers "what is this made OF" — you see one bar and
// read off proportions. Cache read vs write vs miss, or accepted vs
// rejected vs pending, are band questions: the parts are a whole, and
// showing them as three separate bars hides that they sum to one.
struct StatBand {
    struct Seg { std::string label; double value = 0; Color hue; };
    std::string      caption;   // dim note above the bar
    std::vector<Seg> segments;
    bool             legend = true;
};

// A braille line plot over the full sheet width.
//
// A sparkline is one row and answers "is it going up". A plot is several
// rows with a labelled scale and answers "by how much, and when" — at 2×4
// dots per cell it has eight times a block chart's resolution, so a real
// curve fits in four terminal rows. Both exist because a stats tab wants
// the cheap one inline in a table and the detailed one as a figure.
struct StatPlot {
    std::string         caption;
    std::vector<double> series;
    int                 rows = 4;      // terminal rows, so 4× dots tall
    std::optional<Color> hue;
    // Fill the region under the curve instead of drawing a bare line.
    //
    // On by default, and that is a judgement about what these plots are
    // FOR. A line says "here is the trend"; a filled area says "here is
    // the quantity", and every series a stats panel plots — tokens per
    // turn, prefix size, latency — is a quantity accumulating against a
    // real zero. The fill also survives a sparse series, where a bare
    // line at braille resolution is a scatter of disconnected pixels.
    bool                filled = true;
    // Right-hand scale labels. The peak is the number a reader needs to
    // interpret every other point, and a plot without it is a shape with
    // no units — decoration rather than a statistic.
    std::string         peak_label;
    std::string         base_label;
};

class StatSheet {
public:
    using Row = std::variant<StatHeading, StatBlank, StatHero, StatEntry,
                             StatBand, StatPlot>;

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
    StatSheet& band(StatBand b) {
        rows_.emplace_back(std::move(b));
        return *this;
    }
    StatSheet& plot(StatPlot p) {
        rows_.emplace_back(std::move(p));
        return *this;
    }

    // Track width for ranked bars, in columns. The default reads at the
    // panel widths this ships at; a narrow surface shrinks it automatically
    // (see the degradation ladder in build()).
    StatSheet& track(int cells) { track_ = cells; return *this; }
    // Left inset, matching the panel's own gutter.
    StatSheet& indent(int cols) { indent_ = cols; return *this; }
    // Columns to leave free on the RIGHT.
    //
    // Only the full-width forms (band, plot, a `wide` entry) can collide
    // with what a host draws at its own right edge — a scrollbar, a border,
    // an inner pad the component was never told about. A ranked bar stops
    // well short and never notices. So this is not a general margin: it is
    // the amount by which "full width" is a lie on this surface, and the
    // host is the only one who knows it.
    StatSheet& reserve_right(int cols) { reserve_ = cols; return *this; }

    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return detail::component([rows = rows_, theme = theme,
                                  track = track_, indent = indent_,
                                  reserve = reserve_]
                                 (int avail_w, int) -> Element {
            // The layout engine hands a component an "unconstrained"
            // sentinel (~1<<24) during auto-height / auto-width MEASURE
            // passes. Every full-width form here derives its geometry from
            // this number, so without a cap a band becomes millions of
            // columns wide, is measured as such, and then paints past the
            // right edge of whatever contains it. 4096 is far past any
            // real terminal while keeping the arithmetic finite.
            //
            // The same trap LineChart documents — which is the tell that it
            // belongs to the engine's contract rather than to either widget.
            constexpr int kMaxWidth = 4096;
            if (avail_w > kMaxWidth) avail_w = kMaxWidth;
            const int usable = avail_w - reserve;
            const int avail  = usable > indent ? usable - indent : 0;

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
                } else if (const auto* bd = std::get_if<StatBand>(&r)) {
                    emit_band(*bd, out, theme, avail, pad);
                    continue;
                } else if (const auto* pl = std::get_if<StatPlot>(&r)) {
                    emit_plot(*pl, out, theme, avail, pad);
                    continue;
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

            // Fixed height, and shrink disabled. The sheet knows exactly
            // how many rows it produced; the parent does not, and a vstack
            // whose children have no basis is free to collapse them under
            // shrink — which showed up as a five-row plot rendering as one
            // row, with the other four silently dropped rather than
            // scrolled. Stating the height is what makes the figure a
            // figure instead of a suggestion.
            const int n = static_cast<int>(out.size());
            BoxElement box;
            box.layout.direction = FlexDirection::Column;
            box.children         = std::move(out);
            box.layout.height     = Dimension::fixed(n);
            box.layout.min_height = Dimension::fixed(n);
            box.layout.basis      = Dimension::fixed(n);
            box.layout.shrink     = 0.0f;
            return Element{std::move(box)};
        }).build();
    }

private:
    // ── Composition band ────────────────────────────────────────────────
    //
    // Segment widths are apportioned by LARGEST REMAINDER, not by
    // independent rounding. Rounding each segment on its own leaves the
    // total a column or two short of the track, so the bar's right edge
    // wobbles between bands and the thing that is supposed to read as "a
    // whole" visibly is not one.
    //
    // Every non-zero segment is guaranteed at least one column, for the
    // same reason a 1% bar is not allowed to round to empty: "a sliver"
    // and "nothing" are different readings.
    static void emit_band(const StatBand& b, std::vector<Element>& out,
                          const StatSheetTheme& theme, int avail,
                          const std::string& pad) {
        double total = 0;
        for (const auto& s : b.segments) total += s.value > 0 ? s.value : 0;
        if (b.segments.empty() || total <= 0 || avail <= 0) return;

        if (!b.caption.empty()) {
            out.push_back(Element{TextElement{
                .content = pad + b.caption,
                .style   = Style{}.with_fg(theme.detail),
                .wrap    = TextWrap::TruncateEnd,
            }});
        }

        const int track = avail;
        std::vector<int>    cols(b.segments.size(), 0);
        std::vector<double> rem(b.segments.size(), 0.0);
        int used = 0;
        for (std::size_t i = 0; i < b.segments.size(); ++i) {
            const double v = b.segments[i].value > 0 ? b.segments[i].value : 0;
            const double exact = v / total * track;
            cols[i] = static_cast<int>(exact);
            if (v > 0 && cols[i] == 0) cols[i] = 1;
            rem[i]  = exact - static_cast<double>(static_cast<int>(exact));
            used   += cols[i];
        }
        // Hand out the leftover columns to the biggest remainders first.
        while (used < track) {
            std::size_t best = 0;
            double best_r = -1.0;
            for (std::size_t i = 0; i < rem.size(); ++i)
                if (rem[i] > best_r) { best_r = rem[i]; best = i; }
            ++cols[best];
            rem[best] = -1.0;
            ++used;
            if (best_r < 0) break;   // nothing left to give to
        }
        while (used > track) {
            std::size_t best = 0;
            int widest = -1;
            for (std::size_t i = 0; i < cols.size(); ++i)
                if (cols[i] > widest) { widest = cols[i]; best = i; }
            if (widest <= 1) break;
            --cols[best];
            --used;
        }

        std::string s = pad;
        std::vector<StyledRun> runs;
        for (std::size_t i = 0; i < b.segments.size(); ++i) {
            if (cols[i] <= 0) continue;
            std::string seg;
            for (int c = 0; c < cols[i]; ++c) seg += stat_detail::kFullBlock;
            runs.push_back(StyledRun{s.size(), seg.size(),
                                     Style{}.with_fg(b.segments[i].hue)});
            s += seg;
        }
        out.push_back(Element{TextElement{
            .content = std::move(s),
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs),
        }});

        if (!b.legend) return;
        // Legend on ONE row: a band with a per-segment legend row is a
        // ranked bar chart wearing a costume, and costs the vertical space
        // the single-bar form exists to save.
        std::string ls = pad;
        std::vector<StyledRun> lruns;
        for (std::size_t i = 0; i < b.segments.size(); ++i) {
            if (b.segments[i].value <= 0) continue;
            if (ls.size() > pad.size()) {
                lruns.push_back(StyledRun{ls.size(), 3, Style{}.with_fg(theme.detail)});
                ls += "   ";
            }
            const std::string dot{stat_detail::kLegendDot};
            lruns.push_back(StyledRun{ls.size(), dot.size(),
                                      Style{}.with_fg(b.segments[i].hue)});
            ls += dot;
            const std::string lbl = " " + b.segments[i].label;
            lruns.push_back(StyledRun{ls.size(), lbl.size(),
                                      Style{}.with_fg(theme.label)});
            ls += lbl;
        }
        out.push_back(Element{TextElement{
            .content = std::move(ls),
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(lruns),
        }});
    }

    // ── Braille plot ─────────────────────────────────────────────────────
    //
    // Two renderings, and the choice is about what the reader is asked to
    // see. A LINE wants braille: 2x4 dots per cell, eight times a block
    // chart's resolution, which is what a fine trend needs. An AREA wants
    // blocks: braille filled solid is a slab of undifferentiated ink where
    // the eye cannot find the surface, while a column of ▂▅█ has a clean
    // top edge and reads as a quantity at a glance.
    //
    // So `filled` picks the primitive, not just a fill flag.
    static void emit_plot(const StatPlot& p, std::vector<Element>& out,
                          const StatSheetTheme& theme, int avail,
                          const std::string& pad) {
        if (p.series.empty() || avail <= 0) return;
        const int rows = p.rows < 1 ? 1 : (p.rows > 16 ? 16 : p.rows);

        // Reserve the scale gutter first: a plot that overruns its labels
        // is worse than one that is two columns narrower.
        const int label_w = std::max(unicode::str_width(p.peak_label),
                                     unicode::str_width(p.base_label));
        const int gutter  = label_w > 0 ? label_w + 1 : 0;
        const int cells   = avail - gutter;
        if (cells <= 0) return;

        if (!p.caption.empty()) {
            out.push_back(Element{TextElement{
                .content = pad + p.caption,
                .style   = Style{}.with_fg(theme.detail),
                .wrap    = TextWrap::TruncateEnd,
            }});
        }

        double hi = 0.0;
        for (double v : p.series) if (v > hi) hi = v;
        if (hi <= 0.0) hi = 1.0;

        const Color hue = p.hue.value_or(theme.bar);
        auto scale_label = [&](int cy) -> const std::string* {
            if (cy == 0)        return &p.peak_label;
            if (cy == rows - 1) return &p.base_label;
            return nullptr;
        };
        auto emit_row = [&](std::string line, int cy) {
            std::string s = pad;
            std::vector<StyledRun> runs;
            runs.push_back(StyledRun{s.size(), line.size(),
                                     Style{}.with_fg(hue)});
            s += line;
            // Scale labels ride the first and last rows, which is where a
            // reader looks for them and costs no extra vertical space.
            const std::string* lab = scale_label(cy);
            if (gutter > 0 && lab && !lab->empty()) {
                const int lw = unicode::str_width(*lab);
                s.append(static_cast<std::size_t>(1 + label_w - lw), ' ');
                runs.push_back(StyledRun{s.size(), lab->size(),
                                         Style{}.with_fg(theme.detail)});
                s += *lab;
            }
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        };

        if (p.filled) {
            // Area: one block column per cell, eighths of a row each, so a
            // `rows`-tall figure has 8*rows levels. Resampled by MAX over
            // the samples a column covers rather than by point sampling —
            // a spike that survives to the screen is the honest reduction
            // when many turns share one column, and a mean would erase
            // exactly the outlier the reader is looking for.
            const std::size_t n = p.series.size();
            const int levels = rows * 8;
            std::vector<int> col(static_cast<std::size_t>(cells), 0);
            for (int x = 0; x < cells; ++x) {
                const std::size_t lo = n * static_cast<std::size_t>(x)
                                     / static_cast<std::size_t>(cells);
                std::size_t hi_i = n * static_cast<std::size_t>(x + 1)
                                 / static_cast<std::size_t>(cells);
                if (hi_i <= lo) hi_i = lo + 1;
                double peak = 0;
                for (std::size_t i = lo; i < hi_i && i < n; ++i)
                    if (p.series[i] > peak) peak = p.series[i];
                int lv = static_cast<int>(peak / hi * levels + 0.5);
                // A non-zero sample never renders as an empty column, the
                // same rule the bars follow: "almost none" and "none" are
                // different readings.
                if (lv == 0 && peak > 0) lv = 1;
                col[static_cast<std::size_t>(x)] = lv > levels ? levels : lv;
            }
            for (int cy = 0; cy < rows; ++cy) {
                // Row cy covers levels [(rows-1-cy)*8, +8).
                const int base = (rows - 1 - cy) * 8;
                std::string line;
                for (int x = 0; x < cells; ++x) {
                    const int in_row = col[static_cast<std::size_t>(x)] - base;
                    if (in_row <= 0)      line += ' ';
                    else if (in_row >= 8) line += stat_detail::kEighthsUp[7];
                    else                  line += stat_detail::kEighthsUp[
                                              static_cast<std::size_t>(in_row - 1)];
                }
                emit_row(std::move(line), cy);
            }
            return;
        }

        const int dot_w = cells * 2;      // 2 dot columns per cell
        const int dot_h = rows  * 4;      // 4 dot rows per cell

        // Resample the series onto the dot grid. Nearest-neighbour, not
        // interpolation: these are measured samples, and inventing points
        // between them draws a curve the data never had.
        //
        // Consecutive points are then JOINED by a vertical stroke. Without
        // it a steep move leaves a visible gap between two dots and the
        // eye reads two unrelated marks rather than one falling line — the
        // plot stops being a line chart and becomes a scatter of noise.
        // The join spans dots the data does not have, which is honest:
        // it asserts continuity between samples, not values between them.
        std::vector<std::uint8_t> grid(
            static_cast<std::size_t>(rows) * static_cast<std::size_t>(cells), 0);
        const std::size_t n = p.series.size();
        auto dot_at = [&](int x, int y) {
            if (x < 0 || x >= dot_w || y < 0 || y >= dot_h) return;
            grid[static_cast<std::size_t>(y / 4)
                     * static_cast<std::size_t>(cells)
                 + static_cast<std::size_t>(x / 2)]
                |= stat_detail::kBrailleDot[y % 4][x % 2];
        };
        auto y_at = [&](int x) {
            const std::size_t idx =
                n == 1 ? 0
                       : static_cast<std::size_t>(
                             static_cast<double>(x) / (dot_w - 1) * (n - 1) + 0.5);
            const double f = p.series[idx < n ? idx : n - 1] / hi;
            int y = dot_h - 1 - static_cast<int>(f * (dot_h - 1) + 0.5);
            if (y < 0) y = 0;
            if (y >= dot_h) y = dot_h - 1;
            return y;
        };
        int prev_y = y_at(0);
        for (int x = 0; x < dot_w; ++x) {
            const int y = y_at(x);
            const int lo = y < prev_y ? y : prev_y;
            const int hi_y = y < prev_y ? prev_y : y;
            for (int yy = lo; yy <= hi_y; ++yy) dot_at(x, yy);
            prev_y = y;
        }

        for (int cy = 0; cy < rows; ++cy) {
            std::string line;
            for (int cx = 0; cx < cells; ++cx)
                line += stat_detail::braille(
                    grid[static_cast<std::size_t>(cy)
                         * static_cast<std::size_t>(cells)
                         + static_cast<std::size_t>(cx)]);
            emit_row(std::move(line), cy);
        }
    }

    std::vector<Row> rows_;
    int track_   = 14;
    int indent_  = 0;
    int reserve_ = 0;
};

}  // namespace maya
