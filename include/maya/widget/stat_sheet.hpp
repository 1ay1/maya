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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
// A column boundary. Inert in single-column mode; in multi-column mode
// everything after it starts a fresh column.
struct StatBreak   {};
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

// A DONUT: the same composition a band shows, drawn as a ring with the
// legend beside it.
//
// Worth having alongside StatBand because the two fail in opposite
// directions. A band is exact and compact — you can read "68% / 12% / 20%"
// off the segment lengths — but a 1-column sliver in a 70-column bar is
// easy to miss entirely. A ring puts the parts around a closed loop where
// the eye compares ANGLES, and a thin wedge against a circle is obvious in
// a way a thin segment against a line is not. It also earns its keep on a
// wide surface: it fills vertical space a band leaves blank.
//
// Drawn on half-block pixels (2 vertical pixels per cell), with the x
// radius doubled so the ring is round rather than an ellipse — terminal
// cells are about twice as tall as they are wide, and a circle plotted in
// cell units comes out squashed.
struct StatDonut {
    struct Seg { std::string label; double value = 0; Color hue; };
    std::string      caption;
    std::vector<Seg> segments;
    // Rows the figure occupies. 7 gives a 14-pixel-tall ring, which is the
    // smallest that still reads as a circle rather than an octagon.
    int              rows = 7;
    // The ceiling `rows` may GROW to on a wide surface. `rows` is the
    // preferred size, not the maximum — a ring that ignores a 200-column
    // terminal wastes the room it was given.
    //
    // Kept close to `rows` on purpose. A donut is a CIRCLE, so growing it
    // horizontally also grows it vertically, and vertical space is the one
    // the panel is actually short of: every row the ring gains is a row of
    // real content pushed below the fold, on a surface that had spare
    // WIDTH and no spare height. Two rows is enough to stop the figure
    // looking marooned without turning a stats tab into a poster.
    int              rows_max = 9;
    // Centre text — the headline the ring is decorating. A donut with an
    // empty middle wastes the one place a reader is already looking.
    std::string      center;
    std::string      center_sub;
};

// A VERTICAL histogram: columns rising from a baseline, with an axis.
//
// The distribution forms differ in what they cost and what they show. A
// Dist section is one ROW per bucket — exact, labelled, and it scrolls
// forever, which is right when the buckets have names worth reading. A
// vertical histogram is one COLUMN per bucket, so the whole distribution
// is a single shape the eye takes in at once: bimodality, skew and a long
// tail are recognisable before a single label is read.
//
// Drawn in braille for the same reason the plots are: at 2x4 dots per
// cell a column has 4x the vertical resolution of a block, so adjacent
// buckets of similar height stay distinguishable instead of both
// rounding to a full block.
struct StatHistogram {
    struct Bucket {
        std::string label;   // axis tick, drawn under the column
        double      value = 0;
    };
    std::string          caption;
    std::vector<Bucket>  buckets;
    int                  rows = 5;   // terminal rows of column height
    std::optional<Color> hue;
    // Columns narrower than this are widened by dropping buckets from the
    // display; wider makes each bar easier to hit with the eye. 3 is the
    // narrowest that still leaves a gap between neighbours.
    int                  col_width = 3;
    // The ceiling `col_width` may grow to when the surface has width to
    // spare. Same contract as StatDonut::rows_max: the declared width is a
    // preference, and unused width is a chart declining to be read.
    int                  col_width_max = 9;
    // Y-axis ticks, TOP ROW FIRST, one per row. Fewer than `rows` labels
    // leaves the remaining rows unlabelled, which is how a caller asks
    // for a sparser axis.
    //
    // A vector rather than a single peak: one number at the top tells you
    // the ceiling and nothing else, so reading any bar means estimating
    // its fraction of a value at the other end of the figure. A tick per
    // row turns that estimate into a lookup.
    //
    // Pre-formatted, like every other number the sheet takes — the widget
    // does not know whether these are milliseconds, bytes or counts.
    std::vector<std::string> y_labels;
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
    // The width past which a number is a layout-engine SENTINEL rather
    // than a terminal. Shared by the render and measure paths so the two
    // agree about what counts as "unconstrained".
    static constexpr int kSentinel = 4096;
    using Row = std::variant<StatHeading, StatBlank, StatHero, StatEntry,
                             StatBand, StatPlot, StatBreak, StatDonut,
                             StatHistogram>;

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
    StatSheet& donut(StatDonut d) {
        rows_.emplace_back(std::move(d));
        return *this;
    }
    StatSheet& histogram(StatHistogram h) {
        rows_.emplace_back(std::move(h));
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

    // Flow the sheet into up to N columns when the surface is wide enough.
    //
    // A stats tab on a 200-column terminal is a narrow ribbon of rows with
    // two thirds of the screen blank, and the reader scrolls for content
    // that would have fitted. The sheet splits itself — balanced by
    // PAINTED HEIGHT so the columns end level (never by row count, which
    // treats a 7-row donut as one row and leaves one column short, and
    // never by section count, which packs one column with the long tables
    // and leaves the other holding a heading).
    //
    // HOW MANY columns is the sheet's decision, not the caller's. It knows
    // its own label and value widths; a caller does not, and a caller-side
    // minimum is a guess that goes stale the moment a row gets a longer
    // label. The sheet searches downward from `max_columns` and takes the
    // first split every slice of which still renders with its track and
    // labels intact — so a wider terminal can only ever gain a column,
    // never lose a chart to one.
    //
    // `min_col_width` is an optional extra FLOOR for a host that knows
    // something the sheet cannot see (a caption it renders above the
    // sheet, say). 0 means "no opinion" — the normal case. Pass
    // max_columns = 1 to disable splitting entirely: a host whose rows are
    // a single ranked list wants that, because splitting a ranked list
    // puts rank 1 and rank 9 side by side and reading order stops meaning
    // anything.
    StatSheet& columns(int max_columns) {
        col_min_w_ = 0;
        col_max_   = max_columns;
        return *this;
    }
    StatSheet& columns(int min_col_width, int max_columns) {
        col_min_w_ = min_col_width;
        col_max_   = max_columns;
        return *this;
    }

    // How many rows the host can show before the reader has to scroll.
    //
    // This is the piece the sheet was missing, and its absence is why
    // splitting looked arbitrary. Columns are not a reward for having
    // width -- they are a RESPONSE TO VERTICAL PRESSURE. A sheet that fits
    // the viewport has nothing to gain by splitting: the reader can
    // already see all of it, and the split only makes their eye travel
    // sideways and back. Width is a CONSTRAINT on splitting (a column too
    // narrow to draw is not a column); height is the REASON for it.
    //
    // Without this the sheet could only ask "can I?", so it split whenever
    // it fit -- which is how a four-row Session tab ended up in two
    // columns on a phone-sized pane while a tab that genuinely overflowed
    // sat in one. With it, the question becomes "must I, and how little
    // can I get away with", which is the same question at every width and
    // on every tab.
    //
    // 0 (the default) means the host has no opinion: fall back to the old
    // behaviour of splitting whenever the width allows.
    StatSheet& height_budget(int rows) { budget_ = rows; return *this; }

    // A hard break: whatever follows starts a new column if the sheet is
    // splitting. Bands and plots are full-width forms and read as broken
    // when they land in a half-width column, so the panel puts them after
    // every column break.
    StatSheet& column_break() {
        rows_.emplace_back(StatBreak{});
        return *this;
    }

    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return detail::component([rows = rows_, theme = theme,
                                  track = track_, indent = indent_,
                                  reserve = reserve_, col_min_w = col_min_w_,
                                  col_max = col_max_, budget = budget_]
                                 (int avail_w, int) -> Element {
            // The layout engine hands a component an "unconstrained"
            // sentinel (~1<<24) during auto-height / auto-width MEASURE
            // passes, and a host that vstacks the sheet measures it at a
            // large finite width too (maya's Panel uses 1<<14). Every
            // full-width form here derives its geometry from this number,
            // so without a guard a band becomes millions of columns wide,
            // is measured as such, and then paints past the right edge of
            // whatever contains it.
            //
            // The guard is a SENTINEL TEST, not a ceiling on real widths.
            // Clamping every width to a fixed 200 also clamped genuine
            // ultrawide terminals, so a 300-column panel drew its border to
            // column 296 and stopped its content at 141 — 150 columns of
            // void inside a fully-drawn frame.
            //
            // For a sentinel the sheet lays out at a NARROW width on
            // purpose. Row count is what the host budgets for scrolling,
            // and narrow is the direction that OVER-reports: more
            // wrapping, more rows, so the host reserves at least as many
            // as the paint pass will produce and the tail stays reachable.
            // Measuring wide under-reports, which is the failure that
            // strands content below an unscrollable viewport.
            //
            // 40, not 80, and the difference is the whole bug. A sheet
            // SPLITS once it is wide enough, and splitting roughly halves
            // its height -- so 80 measured a two-column sheet at 9 rows
            // while the 76-column paint pass, one column short of a split,
            // painted 19. The host budgeted 9, the panel clipped at its
            // viewport, and max_y said there was nothing to scroll to: the
            // Cache tab's Tokens and Rates sections were simply gone.
            //
            // Picking a narrower number is not the fix though, because
            // "too narrow to split" is not a property of a WIDTH -- it
            // depends on the content, and a sheet of short labels splits
            // happily at 32. Nor is any single width enough on its own:
            // between 40 and 76 this sheet's height climbs from 17 to 19
            // as rows wrap differently, so a measure taken at one width
            // still strands rows at another.
            //
            // The honest answer is to report the TALLEST layout the sheet
            // has -- one column, at the narrowest width it will ever be
            // asked to paint. Height falls monotonically as width grows
            // (less wrapping, then a split that halves it), so the narrow
            // single-column case is a true ceiling for every width, and a
            // ceiling is exactly what a scroll budget needs. Over-
            // reserving costs a few blank rows at the bottom; under-
            // reserving loses content with no way to reach it.
            constexpr int kMeasureWidth = 40;     // the narrowest we support
            const bool measuring = avail_w >= kSentinel;
            if (measuring) avail_w = kMeasureWidth;
            if (avail_w < 1) avail_w = 1;

            // ── Column split (LEVEL 2) ───────────────────────────────
            //
            // TWO questions, and they are not the same one:
            //
            //   MUST I split?   a HEIGHT question. Columns exist to relieve
            //                   vertical pressure, so a sheet that already
            //                   fits its viewport gains nothing by splitting
            //                   and pays for it in eye travel.
            //   CAN I split?    a WIDTH question. A column too narrow to
            //                   draw a track and a full label in is not a
            //                   column, it is damage.
            //
            // Width was the only one being asked, which is why splitting
            // looked arbitrary: a short Session tab split on a phone-sized
            // pane because it COULD, while a genuinely overflowing tab sat
            // in one column because a slice came up a few cells short.
            //
            // Both questions live in decide(), which the measure pass runs
            // too -- so what gets budgeted and what gets painted cannot
            // disagree.
            const int full = avail_w - reserve;
            auto layout = measuring
                        ? Layout{}
                        : decide(rows, theme, track, indent, reserve,
                                 col_min_w, col_max, budget, avail_w);

            if (layout.slices.size() > 1) {
                const int cols = static_cast<int>(layout.slices.size());
                const int inner = (full - kColGap * (cols - 1)) / cols;
                std::vector<Element> els;
                int tallest = 0;
                for (auto& sl : layout.slices) {
                    auto col = render_slice(sl, theme, track, indent, inner);
                    if (col.second > tallest) tallest = col.second;
                    els.push_back(std::move(col.first));
                }
                BoxElement row;
                row.layout.direction = FlexDirection::Row;
                row.layout.gap       = kColGap;
                row.layout.height     = Dimension::fixed(tallest);
                row.layout.min_height = Dimension::fixed(tallest);
                row.layout.basis      = Dimension::fixed(tallest);
                row.layout.shrink     = 0.0f;
                for (auto& c : els) {
                    BoxElement cell;
                    cell.layout.direction = FlexDirection::Column;
                    cell.layout.width     = Dimension::fixed(inner);
                    cell.layout.basis     = Dimension::fixed(inner);
                    cell.layout.shrink    = 0.0f;
                    cell.children.push_back(std::move(c));
                    row.children.push_back(Element{std::move(cell)});
                }
                return Element{std::move(row)};
            }

            // One column: at the width decide() already settled on, so
            // what was measured is what gets drawn.
            const int use = measuring ? single_surface(rows, track, full)
                                      : layout.surface;
            return render_slice(rows, theme, track, indent, use).first;
        })
        // Report the sheet's TALLEST layout as its natural height.
        //
        // Without this the framework auto-measures by rendering at the
        // host's sentinel width, and a host that measures wide gets the
        // height of a SPLIT sheet -- roughly half the real thing. maya's
        // Panel budgets scrolling from exactly that number, so on any
        // width too narrow to split, the surplus rows were unreachable:
        // the body clipped at the viewport and max_y said there was
        // nothing to scroll to. The Cache tab lost its Tokens and Rates
        // sections outright, with no scrollbar to suggest they existed.
        //
        // Height falls monotonically as width grows -- less wrapping,
        // then a split that halves it -- so ONE COLUMN AT THE NARROWEST
        // SUPPORTED WIDTH is a true ceiling for every width the sheet
        // might be painted at. A scroll budget wants a ceiling: over-
        // reserving costs a few blank rows at the bottom, while under-
        // reserving loses content with no way to reach it.
        .measure([rows = rows_, theme = theme, track = track_,
                  indent = indent_, reserve = reserve_,
                  col_min_w = col_min_w_, col_max = col_max_,
                  budget = budget_](int max_width) -> Size {
            constexpr int kNarrowest = 40;
            int w = max_width;
            if (w >= kSentinel || w <= 0) w = kNarrowest;

            // The height that will actually be PAINTED.
            //
            // This has to run the same decision the layout pass runs,
            // because the two now disagree in both directions if it does
            // not. Reporting the unsplit height for a sheet that WILL
            // split over-reserves — harmless but sloppy — while reporting
            // a split height for a sheet that will NOT split strands the
            // rows past the budget, which is the bug this callback was
            // added to fix. One shared helper, one answer.
            auto painted = [&](int surface) {
                return laid_out_height(rows, theme, track, indent, reserve,
                                       col_min_w, col_max, budget, surface);
            };

            // Height is not monotonic in width: it falls as wrapping
            // eases, but a narrower sheet can also fit more on a line and
            // so wrap less, and a split roughly halves it again. Guessing
            // one width therefore still strands rows at another, so take
            // the maximum over the band the sheet can be painted at.
            //
            // The band is only consulted for the SENTINEL, though. When a
            // real width is on offer, that is the width the sheet will be
            // painted at, and its layout there -- split or not -- is the
            // only honest answer. Maxing it against a band of hypothetical
            // narrow layouts reported the UNSPLIT height for a sheet that
            // was about to split, so the panel reserved 39 rows for a
            // 20-row layout and published a scrollbar for content that was
            // entirely on screen.
            if (max_width < kSentinel && max_width > 0)
                return Size{Columns{max_width}, Rows{painted(max_width)}};

            // At the sentinel the real width is unknown. The old answer
            // was the worst case over a band of narrow widths, on the
            // theory that over-reserving is always safe -- and for a sheet
            // that cannot split, it is.
            //
            // Once splitting depends on a height BUDGET that changes with
            // the width, it stops being safe and becomes actively wrong.
            // A narrow probe cannot fit two columns, so it reports the
            // unsplit height; the sheet then paints at the host's real
            // width, splits, and comes out half that tall. maya's Panel
            // takes the measured number as gospel, so it reserved 39 rows
            // for a 20-row layout and published a scrollbar over content
            // that was entirely on screen -- the two columns AND a
            // scrollbar this whole chain of fixes has been chasing.
            //
            // So the band is capped at what the sheet would do with the
            // room a real terminal offers. Still a worst case over widths
            // the host might actually use, but no longer dominated by
            // hypothetical narrow ones the sheet will never see.
            int tallest = 1;
            for (int probe = 40; probe <= 96; probe += 4)
                tallest = std::max(tallest, painted(probe));
            if (budget > 0) tallest = std::min(tallest, painted(96));

            // Report the WIDTH WE WERE ASKED ABOUT, never the probe width.
            //
            // This callback exists to answer a question about HEIGHT, and
            // returning the probe width told the layout engine the sheet's
            // natural width was 40 -- so it was sized to 40 columns and
            // stayed there on a 300-column terminal, every bar frozen at
            // the same 16 cells. A height answer must not smuggle in a
            // width decision.
            return Size{Columns{max_width}, Rows{tallest}};
        })
        .build();
    }

private:
    // ── The column decision, in ONE place ────────────────────────────────
    //
    // Both the paint pass and the measure pass need the answer to "how
    // many columns, and how tall does that come out". They used to compute
    // it separately, and separate copies of a decision drift: the measure
    // side reported a single-column height for a sheet the paint side
    // split, so the panel budgeted scrolling for rows that were never
    // painted -- and, before that, the reverse, which stranded rows that
    // WERE painted below an unscrollable viewport.
    //
    // Returns the chosen slices (empty = one column) and the height that
    // choice produces.
    struct Layout {
        std::vector<std::vector<Row>> slices;   // empty => single column
        int height  = 0;
        int surface = 0;   // the width a SINGLE column is painted at
    };

    static constexpr int kColGap = 3;

    // The width one column should actually use.
    //
    // A sheet the splitter declined to divide -- one ranked list, three
    // rows -- laid out at the full width put "By model" and its number at
    // opposite ends of a 220-column terminal. Filling the surface is not
    // the goal; being READ is, and a measure has a length past which it
    // stops being one.
    //
    // Only past a real surplus, though, and even then it keeps growing
    // with the surface at half rate: clamping every sheet to its natural
    // width left a 64-column pane ending a third short of its own border,
    // which reads as broken rather than as restraint.
    [[nodiscard]] static int single_surface(const std::vector<Row>& rows,
                                            int track, int full) {
        const int want = natural_width(rows, track);
        if (want > 0 && full > want * 2) return want + (full - want) / 2;
        return full;
    }

    [[nodiscard]] static Layout
    decide(const std::vector<Row>& rows, const StatSheetTheme& theme,
           int track, int indent, int reserve, int col_min_w, int col_max,
           int budget, int avail_w) {
        Layout out;
        const int full = avail_w - reserve;
        if (full <= 0) return out;

        // One column is the baseline and the preference. Every extra
        // column costs the reader a sideways journey, so it has to be
        // earned by content that does not otherwise fit.
        //
        // Measured at the width it will actually be PAINTED at, not at the
        // raw surface: the two differ once the sheet declines width, and a
        // baseline measured against a different width than the one drawn
        // is the same measure-says-one-thing-paint-does-another bug this
        // whole helper exists to prevent.
        out.surface = single_surface(rows, track, full);
        out.height  = render_slice(rows, theme, track, indent, out.surface).second;
        if (col_max <= 1) return out;

        // The track one column affords, as the yardstick a split has to
        // measure up to below.
        const Fit base_fit = fit(rows, track, std::max(0, out.surface - indent));
        const int  baseline_track = base_fit.any_bar ? base_fit.bar_w : 0;

        // MUST we? Columns relieve VERTICAL pressure. A sheet that already
        // fits the viewport gains nothing by splitting, whatever width it
        // happens to have been given -- which is the whole reason a short
        // tab used to come out in two columns on a phone-sized pane.
        //
        // With no budget the host has no opinion, so fall back to the old
        // behaviour and split whenever the width allows.
        if (budget > 0 && out.height <= budget) return out;

        int cap = col_max;
        if (col_min_w > 0) {
            const int afford = (full + kColGap) / (col_min_w + kColGap);
            if (afford < cap) cap = afford;
        }

        // Upward from two: the FEWEST columns that solve the problem, not
        // the most the surface could hold.
        for (int n = 2; n <= cap; ++n) {
            auto slices = split_columns(rows, n);
            if (static_cast<int>(slices.size()) < n) continue;
            const int cols = static_cast<int>(slices.size());
            const int inner = (full - kColGap * (cols - 1)) / cols;
            if (inner <= indent) continue;
            const int usable = inner - indent;

            // CAN we? Every slice has to survive level 1 with its track
            // and its labels intact; a column too narrow to draw is not a
            // column, it is damage.
            bool ok = true;
            int tallest = 0;
            int thinnest_track = std::numeric_limits<int>::max();
            for (const auto& sl : slices) {
                const Fit f = fit(sl, track, usable);
                if (!f.ok(usable)) { ok = false; break; }
                if (f.any_bar) thinnest_track = std::min(thinnest_track, f.bar_w);
                tallest = std::max(
                    tallest, render_slice(sl, theme, track, indent, inner).second);
            }
            if (!ok) continue;

            // Would it GUT the charts?
            //
            // kTrackMin is a survival floor -- the width below which a bar
            // stops being a bar at all. Passing it is not the same as being
            // worth having: a table whose labels are long (`git_status` is
            // ten characters) hands the track whatever the label column
            // does not want, so a half-width slice can leave a legal but
            // useless 9-cell bar where one column had 24. Every row then
            // reads as the same stub and the numbers do all the work,
            // which is precisely the comparison the chart existed to make.
            //
            // So a split is also judged against the ALTERNATIVE, not just
            // against the floor. Losing more than half the track is too
            // much to pay for shorter columns; the reader is better served
            // by one column with a chart they can actually read.
            if (thinnest_track != std::numeric_limits<int>::max()
                && baseline_track > 0
                && thinnest_track * 2 < baseline_track) continue;

            // Did it HELP ENOUGH?
            //
            // Two different bars, because there are two different reasons
            // to be splitting:
            //
            //  - Under a budget, the split's job is to make the content
            //    FIT. A split that shortens 33 rows to 29 against an
            //    18-row viewport has not done that job: the reader still
            //    scrolls, and now their eye has to travel sideways as
            //    well. That is strictly worse than one honest column, and
            //    it is what put a Tools tab into two columns on a phone
            //    while still showing a scrollbar.
            //
            //  - With no budget the host has no opinion about height, so
            //    any real shortening is a win and the old bar applies.
            //
            // A split that cannot reach the budget is not "partial
            // progress" -- it is the cost with none of the benefit. Skip
            // it and let a larger `n` try; if none of them fit, the sheet
            // stays in one column and scrolls, which is the honest
            // rendering of content that genuinely does not fit.
            if (tallest >= out.height) continue;
            if (budget > 0 && tallest > budget) continue;

            out.slices = std::move(slices);
            out.height = tallest;
            if (budget <= 0 || tallest <= budget) break;
        }
        return out;
    }

    // The painted height for a given surface -- the measure pass's view of
    // exactly what the paint pass will do.
    [[nodiscard]] static int
    laid_out_height(const std::vector<Row>& rows, const StatSheetTheme& theme,
                    int track, int indent, int reserve, int col_min_w,
                    int col_max, int budget, int avail_w) {
        return std::max(1, decide(rows, theme, track, indent, reserve,
                                  col_min_w, col_max, budget, avail_w).height);
    }

    // ── The two levels of responsiveness ─────────────────────────────────
    //
    // A sheet answers TWO questions when the width changes, and they are
    // not independent:
    //
    //   Level 1  given ONE column of width W, how do label / track / value
    //            / note divide it?
    //   Level 2  given the full width, HOW MANY columns should there be?
    //
    // The bug this structure exists to kill is level 2 deciding without
    // consulting level 1. When the column count came from a caller-supplied
    // magic minimum, a width could buy a second column that level 1 then
    // could not afford to draw a track in — so widening the terminal DELETED
    // the charts, and kept deleting them for a 16-column band before there
    // was enough room again. Non-monotonic layout is not a rounding error;
    // it reads as the program malfunctioning.
    //
    // So both levels call `fit()`. Level 1 calls it to render. Level 2 calls
    // it to CHOOSE: it only takes a split whose every slice still fits with
    // its track intact and its labels untruncated. The column count is then
    // monotonic in width by construction — more room can never cost you a
    // figure, because the extra column has to prove it earns itself first.

    // A bar narrower than this is not a chart. At 8 cells the smallest
    // visible difference is ~12%, which is about the floor for "these two
    // rows differ" to be legible at a glance; below it the fill is noise
    // and the number beside it is doing all the work.
    static constexpr int kTrackMin = 8;
    // And past this a bar stops reading as a proportion and becomes a rule
    // running off toward its value. Growth is capped, not unbounded: the
    // point of a wide terminal is more COLUMNS of content, not one
    // ludicrously long bar.
    static constexpr int kTrackMax = 40;
    // The track width a row would CHOOSE if width were free — the basis for
    // the sheet's natural width. Comfortably readable without being the
    // 40-cell maximum, which is a ceiling for growth, not a preference.
    static constexpr int kTrackWant = 24;
    // What a full-width form wants before extra columns stop helping it.
    // A curve needs horizontal room to be a curve and a band needs its
    // segments to be distinguishable; past these they are just wider.
    static constexpr int kPlotWant = 64;
    static constexpr int kBandWant = 56;
    // The gap between a sheet's own columns (label│track│value│note).
    static constexpr int kGap = 2;

    // The resolved geometry of one column, and whether it is worth having.
    struct Fit {
        int  label_w   = 0;
        int  bar_w     = 0;
        int  value_w   = 0;
        int  detail_w  = 0;
        int  text_w    = 0;    // widest heading / hero line
        bool any_bar   = false;   // some row WANTS a chart
        bool truncated = false;   // a label had to be cut to fit
        bool has_rows  = false;   // any StatEntry at all

        // The quality floor level 2 tests a candidate split against.
        //
        // A slice of pure figures (a donut, a plot) always passes: it has
        // no label/track geometry to lose, and refusing to split on its
        // account would pin whole tabs to one column.
        [[nodiscard]] bool ok(int avail) const noexcept {
            // Prose is measured whether or not the slice has table rows.
            // A heading or a hero caption is a SENTENCE — "75% of 8 routed
            // turns ran below the Strateg…" is not a smaller version of
            // the fact, it is the fact with the answer cut off — so a
            // split that would clip one is not a split worth having.
            if (text_w > avail) return false;
            if (!has_rows)          return true;
            if (truncated)          return false;
            if (any_bar && bar_w < kTrackMin) return false;
            return true;
        }
    };

    // LEVEL 1. Solve one column's geometry for `avail` usable columns.
    //
    // Shrinks AND grows. The shed ladder was always here; what was missing
    // was the other arm — the track was a frozen constant, so a slice handed
    // 250 columns laid out exactly like one handed 60 and simply padded the
    // difference. A chart that ignores the space it was given is the same
    // failure as one that overflows it, just quieter.
    [[nodiscard]] static Fit
    fit(const std::vector<Row>& rows, int track_pref, int avail) {
        Fit f;
        for (const auto& r : rows) {
            // Prose first: a heading and a hero are full-width lines that
            // owe nothing to the table geometry, but they still have to
            // FIT, and nothing was measuring them.
            if (const auto* h = std::get_if<StatHeading>(&r)) {
                f.text_w = std::max(f.text_w, unicode::str_width(h->text));
                continue;
            }
            if (const auto* hero = std::get_if<StatHero>(&r)) {
                int w = unicode::str_width(hero->value);
                if (!hero->caption.empty())
                    w += 1 + unicode::str_width(hero->caption);
                f.text_w = std::max(f.text_w, w);
                continue;
            }
            const auto* e = std::get_if<StatEntry>(&r);
            if (!e) continue;
            f.has_rows = true;
            f.label_w  = std::max(f.label_w,  unicode::str_width(e->label));
            f.value_w  = std::max(f.value_w,  unicode::str_width(e->value));
            f.detail_w = std::max(f.detail_w, unicode::str_width(e->detail));
            if (e->share >= 0.0 || !e->spark.empty()) f.any_bar = true;
        }
        if (!f.has_rows) return f;

        f.bar_w = f.any_bar ? std::max(track_pref, kTrackMin) : 0;

        auto total = [&] {
            int t = f.label_w + f.value_w;
            if (f.bar_w)    t += f.bar_w + kGap;
            if (f.detail_w) t += f.detail_w + kGap;
            return t + kGap;
        };

        // ── Shed, in the order that loses the least ──────────────────────
        //
        // A stats row is label + chart + number + note, and they are not
        // equally load-bearing. The NUMBER is the datum, so it is last to
        // go; the note is redundant with it, so it goes first; the chart is
        // a comparison aid that only pays for itself with room to be
        // proportional, so it goes second and shrinks before it vanishes.
        // The label is truncated rather than dropped, because a number
        // whose subject is unknown is not a statistic.
        if (total() > avail && f.detail_w) f.detail_w = 0;
        while (total() > avail && f.bar_w > 4) --f.bar_w;
        if (total() > avail && f.bar_w) f.bar_w = 0;
        if (total() > avail) {
            const int slack = total() - avail;
            const int want  = f.label_w - slack;
            f.label_w  = std::max(4, want);
            f.truncated = true;
        }

        // ── Or grow, if there is room left over ──────────────────────────
        //
        // Spare columns go to the track — not to the gaps, and not to the
        // label, which is already as wide as its widest entry. But the
        // track takes a SHARE of the surface rather than all the slack it
        // can reach: a bar that swallows every spare column on a 100-wide
        // terminal is 40 cells of chart against a 5-cell number, which
        // reads as a progress bar that forgot to stop, and it leaves the
        // value stranded in the middle of the row with dead air past it.
        //
        // A third of the column is the ratio that keeps a stats row
        // reading as label · chart · number instead of as a chart with
        // annotations. Clamped both ways: never below the legibility floor
        // that makes a bar a bar, never past the point where length stops
        // encoding proportion.
        if (f.bar_w > 0) {
            const int slack = avail - total();
            if (slack > 0) {
                // A third of the column, but never less than the track the
                // sheet's own natural width was computed against — those
                // two numbers have to agree or they fight: natural_width
                // asks for room for a kTrackWant-cell bar, and a growth
                // rule that then only grants avail/3 hands back a bar
                // narrower than the width was granted for, which shows up
                // as a sheet that got exactly what it asked for and drew
                // it smaller anyway.
                const int want = std::clamp(std::max(avail / 3, kTrackWant),
                                            kTrackMin, kTrackMax);
                if (want > f.bar_w)
                    f.bar_w = std::min(f.bar_w + slack, want);
            }
        }
        return f;
    }

    // The width this slice would use if nothing constrained it.
    //
    // Label, a comfortable track, the value and its note, plus the gaps
    // between them — the point past which extra columns buy the reader
    // nothing. Prose has the same property and typography has always
    // known it: a line can be too long to read, and a 3-row table stretched
    // across 220 columns makes the eye travel a hand's width from "By
    // model" to the number it belongs to.
    //
    // Figures are asked what they want too, so a tab that is mostly a ring
    // does not get clipped to the width of the little table beside it.
    [[nodiscard]] static int natural_width(const std::vector<Row>& rows,
                                           int track) {
        Fit f;
        for (const auto& r : rows) {
            if (const auto* h = std::get_if<StatHeading>(&r)) {
                f.text_w = std::max(f.text_w, unicode::str_width(h->text));
                continue;
            }
            if (const auto* hero = std::get_if<StatHero>(&r)) {
                int w = unicode::str_width(hero->value);
                if (!hero->caption.empty())
                    w += 1 + unicode::str_width(hero->caption);
                f.text_w = std::max(f.text_w, w);
                continue;
            }
            const auto* e = std::get_if<StatEntry>(&r);
            if (!e) continue;
            f.has_rows = true;
            f.label_w  = std::max(f.label_w,  unicode::str_width(e->label));
            f.value_w  = std::max(f.value_w,  unicode::str_width(e->value));
            f.detail_w = std::max(f.detail_w, unicode::str_width(e->detail));
            if (e->share >= 0.0 || !e->spark.empty()) f.any_bar = true;
        }

        int w = f.text_w;
        if (f.has_rows) {
            int t = f.label_w + f.value_w + kGap;
            if (f.any_bar)  t += kTrackWant + kGap;
            if (f.detail_w) t += f.detail_w + kGap;
            w = std::max(w, t);
        }

        // A figure's own appetite. A donut is its grown ring plus a
        // legend; a histogram is every bucket at its preferred width plus
        // the axis gutter; a plot wants room for the curve to be a curve.
        for (const auto& r : rows) {
            if (const auto* d = std::get_if<StatDonut>(&r)) {
                int lb = 0;
                for (const auto& s : d->segments)
                    lb = std::max(lb, unicode::str_width(s.label) + 8);
                w = std::max(w, d->rows_max * 2 + 3 + std::max(lb, 14));
            } else if (const auto* hg = std::get_if<StatHistogram>(&r)) {
                int lab = 0;
                for (const auto& l : hg->y_labels)
                    lab = std::max(lab, unicode::str_width(l));
                w = std::max(w, static_cast<int>(hg->buckets.size())
                                    * hg->col_width
                                + (lab ? lab + 1 : 0));
            } else if (const auto* p = std::get_if<StatPlot>(&r)) {
                const int lab = std::max(unicode::str_width(p->peak_label),
                                         unicode::str_width(p->base_label));
                w = std::max(w, kPlotWant + (lab ? lab + 1 : 0));
            } else if (const auto* b = std::get_if<StatBand>(&r)) {
                w = std::max(w, kBandWant);
            }
        }
        return w;
    }

    // How many terminal rows one Row actually PAINTS.
    //
    // The split used to count every variant as 1, but a donut is 7 rows, a
    // plot is 4 and a histogram is its bars plus an axis. Counting a figure
    // as one row meant a column holding two of them measured "short" and
    // kept being handed more content, which is why a split tab came out
    // with one column ending halfway up the panel and the other running to
    // the bottom. Balance the thing the reader sees — height — not the
    // number of entries in a vector.
    [[nodiscard]] static int row_height(const Row& r) {
        if (const auto* d = std::get_if<StatDonut>(&r)) {
            // rows_max, not rows. `rows` is the caller's PREFERENCE and
            // emit_donut grows past it when the surface allows, so
            // counting the preference under-reports every wide layout by
            // the difference -- and an under-reported height is a split
            // the search believes fits when it does not.
            const int base = d->rows < 3 ? 3 : d->rows;
            return std::max(base, d->rows_max) + (d->caption.empty() ? 0 : 1);
        }
        if (const auto* p = std::get_if<StatPlot>(&r))
            return (p->rows < 1 ? 1 : p->rows) + (p->caption.empty() ? 0 : 1);
        if (const auto* h = std::get_if<StatHistogram>(&r)) {
            // Bars, plus the caption, plus the BASELINE rule, plus the
            // tick row. The baseline was missed: emit_histogram always
            // draws an axis under the bars (a histogram floating with no
            // zero has no scale), so every histogram was one row taller
            // than the balance logic believed.
            int n = (h->rows < 2 ? 2 : h->rows) + (h->caption.empty() ? 0 : 1);
            ++n;                                   // the baseline rule
            for (const auto& b : h->buckets)
                if (!b.label.empty()) { ++n; break; }
            return n;
        }
        if (const auto* b = std::get_if<StatBand>(&r)) {
            int n = 1 + (b->caption.empty() ? 0 : 1);
            if (b->legend) ++n;
            return n;
        }
        if (const auto* hero = std::get_if<StatHero>(&r))
            return 1 + (hero->caption.empty() ? 0 : 1);
        if (std::holds_alternative<StatBreak>(r)) return 0;
        return 1;
    }

    // Split the row list into `ncols` slices of roughly equal HEIGHT.
    //
    // By row count, never by section count: sections differ wildly in
    // length (a two-row Time table against a nine-bucket distribution),
    // and splitting on section boundaries alone leaves one column twice
    // the height of the other — which looks like a layout bug rather than
    // a choice.
    //
    // Breaks only ever land on a section boundary though, because a
    // heading orphaned from its rows is worse than an uneven column. So
    // the target is height and the boundaries are structural: walk the
    // sections, start a new column when the current one has passed its
    // share.
    [[nodiscard]] static std::vector<std::vector<Row>>
    split_columns(const std::vector<Row>& rows, int ncols) {
        // Section boundaries: a heading, or an explicit break.
        std::vector<std::size_t> starts{0};
        for (std::size_t i = 1; i < rows.size(); ++i)
            if (std::holds_alternative<StatHeading>(rows[i])
                || std::holds_alternative<StatBreak>(rows[i]))
                starts.push_back(i);
        if (starts.size() < 2) return {rows};
        starts.push_back(rows.size());

        int total = 0;
        for (const auto& r : rows) total += row_height(r);
        if (total <= 0) return {rows};

        // ── Choose the boundaries, rather than stumbling into them ──────
        //
        // The sections are a sequence and their order is meaning, so a
        // column split is a LINEAR PARTITION: pick ncols-1 cut points that
        // minimise the tallest resulting column. A single greedy pass
        // cannot do this — it commits to a cut before it has seen the
        // sections that cut has to pay for, which is why a tab whose last
        // section was tall came out with one column ending halfway up the
        // panel and the other running past the fold.
        //
        // There are a handful of sections on the widest tab, so the exact
        // answer is affordable: walk every combination of cut points and
        // keep the best. No heuristic to be wrong at the edges.
        const int nsec = static_cast<int>(starts.size()) - 1;
        if (nsec < ncols) ncols = nsec;
        if (ncols < 2) return {rows};

        std::vector<int> sec_h(static_cast<std::size_t>(nsec), 0);
        for (int s = 0; s < nsec; ++s)
            for (std::size_t i = starts[static_cast<std::size_t>(s)];
                 i < starts[static_cast<std::size_t>(s) + 1]; ++i)
                sec_h[static_cast<std::size_t>(s)] += row_height(rows[i]);

        // cuts[k] = index of the first SECTION in column k+1.
        std::vector<int> cuts(static_cast<std::size_t>(ncols - 1), 0);
        std::vector<int> best_cuts;
        int best_cost = std::numeric_limits<int>::max();

        // A section marked with an explicit break WANTS to start a column;
        // honouring it is worth a little imbalance, but not an empty
        // column, so it is priced as a bonus rather than a constraint.
        std::vector<char> wants_break(static_cast<std::size_t>(nsec), 0);
        for (int s = 0; s < nsec; ++s)
            wants_break[static_cast<std::size_t>(s)] =
                std::holds_alternative<StatBreak>(
                    rows[starts[static_cast<std::size_t>(s)]]) ? 1 : 0;

        const std::function<void(int, int)> search = [&](int k, int from) {
            if (k == ncols - 1) {
                int tallest = 0, shortest = std::numeric_limits<int>::max();
                int prev = 0, bonus = 0;
                for (int c = 0; c < ncols; ++c) {
                    const int end = c + 1 < ncols
                                  ? cuts[static_cast<std::size_t>(c)] : nsec;
                    int h = 0;
                    for (int s = prev; s < end; ++s)
                        h += sec_h[static_cast<std::size_t>(s)];
                    if (h <= 0) return;          // never an empty column
                    if (h > tallest)  tallest  = h;
                    if (h < shortest) shortest = h;
                    if (c > 0 && wants_break[static_cast<std::size_t>(prev)])
                        ++bonus;
                    prev = end;
                }
                // A column has to be worth the eye's trip across the gap.
                //
                // Minimising the tallest column alone is satisfied by a
                // one-row column beside an eight-row one — formally the
                // best partition available, and visibly a mistake: the
                // reader gets a lone number marooned in half the panel.
                //
                // What a split BUYS is the height it removes from the
                // tallest column, and for a two-way split that saving is
                // exactly the shortest column. So "lopsided" and "bought
                // us nothing" are the same measurement read two ways, and
                // one test covers both: when the short column is a third
                // of the tall one, the sheet has taken on a ragged layout
                // to save a row or two of scrolling. Decline it and let
                // the content run in one honest column.
                if (shortest * 3 < tallest) return;
                const int cost = tallest - bonus;
                if (cost < best_cost) { best_cost = cost; best_cuts = cuts; }
                return;
            }
            for (int s = from; s <= nsec - (ncols - 1 - k); ++s) {
                cuts[static_cast<std::size_t>(k)] = s;
                search(k + 1, s + 1);
            }
        };
        search(0, 1);
        if (best_cuts.empty()) return {rows};

        std::vector<std::vector<Row>> out;
        int prev = 0;
        for (int c = 0; c < ncols; ++c) {
            const int end = c + 1 < ncols
                          ? best_cuts[static_cast<std::size_t>(c)] : nsec;
            std::vector<Row> cur;
            for (int s = prev; s < end; ++s) {
                for (std::size_t i = starts[static_cast<std::size_t>(s)];
                     i < starts[static_cast<std::size_t>(s) + 1]; ++i) {
                    // A break is a marker, not content, and a leading blank
                    // in a fresh column is the same stray whitespace the
                    // sheet already refuses at the top.
                    if (std::holds_alternative<StatBreak>(rows[i])) continue;
                    if (cur.empty()
                        && std::holds_alternative<StatBlank>(rows[i])) continue;
                    cur.push_back(rows[i]);
                }
            }
            if (!cur.empty()) out.push_back(std::move(cur));
            prev = end;
        }
        return out;
    }

    // Render one slice at `avail_w`. Returns the element and its row count,
    // so a multi-column layout can size the containing row to the tallest.
    [[nodiscard]] static std::pair<Element, int>
    render_slice(const std::vector<Row>& rows, const StatSheetTheme& theme,
                 int track, int indent, int avail_w) {
        {
            const int usable = avail_w;
            const int avail  = usable > indent ? usable - indent : 0;

            // ── Geometry, from the shared solver (LEVEL 1) ─────────────
            //
            // The same call level 2 used to VET this slice's width now
            // resolves it for painting. One solver, so what the column
            // count was chosen on and what actually gets drawn can never
            // disagree — which is exactly how the dead band got in when
            // they were two separate ladders.
            const Fit g = fit(rows, track, avail);
            int label_w  = g.label_w;
            int value_w  = g.value_w;
            int detail_w = g.detail_w;
            int bar_w    = g.bar_w;

            // How much a figure in this slice may grow.
            //
            // Growth costs ROWS as well as columns for anything round, and
            // rows are what the panel is short of. A slice that is already
            // most of a screen tall cannot afford a bigger ring — the
            // rows it gains come straight out of the sections below it,
            // which is how a narrow Cache tab traded its Tokens and Rates
            // tables for a slightly larger circle.
            //
            // So the allowance is what is LEFT of a screen after the
            // slice's other content, and a crowded slice gets none: its
            // figures render at exactly the size the caller asked for,
            // which is the size that fits.
            constexpr int kScreen = 24;
            int fixed_h = 0;
            for (const auto& r : rows)
                if (!std::holds_alternative<StatDonut>(r))
                    fixed_h += row_height(r);
            const int grow_max = std::max(0, kScreen - fixed_h);

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
                } else if (std::holds_alternative<StatBreak>(r)) {
                    // A column marker, not content. In single-column mode
                    // it draws nothing at all — emitting even a blank row
                    // would make a sheet's height depend on whether it
                    // happened to split, which is a layout that shifts
                    // when the terminal is resized past a breakpoint.
                    continue;
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
                } else if (const auto* dn = std::get_if<StatDonut>(&r)) {
                    emit_donut(*dn, out, theme, avail, pad, grow_max);
                    continue;
                } else if (const auto* hg = std::get_if<StatHistogram>(&r)) {
                    emit_histogram(*hg, out, theme, avail, pad);
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
            // The height this slice PAINTS.
            //
            // Not out.size(). Every row emitted here is one terminal line
            // EXCEPT the figures: a donut is a single element that paints
            // thirteen rows, a plot four, a histogram its bars plus an
            // axis. Counting elements told the column search that a slice
            // holding two figures was four rows tall when it was thirty,
            // so the search happily "fitted" a split into a budget it blew
            // by a factor of three -- and the reader got two columns AND a
            // scrollbar, which is the cost of splitting with none of the
            // benefit.
            //
            // row_height() already knows each variant's painted height for
            // the balance logic; the count has to use the same number or
            // the two disagree about what they are balancing.
            int n = 0;
            for (const auto& r : rows) n += row_height(r);
            if (n < static_cast<int>(out.size())) n = static_cast<int>(out.size());
            BoxElement box;
            box.layout.direction = FlexDirection::Column;
            box.children         = std::move(out);
            box.layout.height     = Dimension::fixed(n);
            box.layout.min_height = Dimension::fixed(n);
            box.layout.basis      = Dimension::fixed(n);
            box.layout.shrink     = 0.0f;
            return std::pair<Element, int>{Element{std::move(box)}, n};
        }
    }
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

    // ── Donut ───────────────────────────────────────────────────────
    //
    // Drawn in BRAILLE, not half-blocks, and the reason is aspect. A
    // half-block cell is 1 dot wide and 2 tall, so a circle needs the x
    // radius doubled to compensate — and the result is a ring built from
    // 1x2 rectangles, each of which can carry two colours (fg for the top
    // pixel, bg for the bottom). At the size a stats panel can afford that
    // reads as a mosaic of coloured tiles rather than as a curve.
    //
    // A braille cell is 2 dots wide and 4 tall. Against a terminal cell's
    // roughly 1:2 aspect that makes the dots very nearly SQUARE, so a
    // circle in dot space is a circle on screen with no correction at all,
    // and the ring is four times denser. Each cell carries one colour, so
    // the shape comes from the dots and the colour from whichever segment
    // owns most of them — which is what makes an arc read as an arc.
    static void emit_donut(const StatDonut& d, std::vector<Element>& out,
                           const StatSheetTheme& theme, int avail,
                           const std::string& pad, int grow_max = 0) {
        double total = 0;
        for (const auto& s : d.segments) total += s.value > 0 ? s.value : 0;
        if (d.segments.empty() || total <= 0 || avail <= 0) return;

        int rows = d.rows < 3 ? 3 : (d.rows > 16 ? 16 : d.rows);
        // Dots are square, so the radius is the same in both axes: a
        // `rows`-tall figure is 2*rows dots in radius and therefore
        // 2*rows CELLS across (2 dot columns per cell).
        int cw = rows * 2;

        // The legend is not optional — an unlabelled ring is a few coloured
        // arcs and no information. When the surface cannot hold both, the
        // RING gives up radius until it can: a smaller circle still shows
        // its angles, while a missing key removes the meaning entirely.
        constexpr int kLegendMin = 14;
        while (cw + 3 + kLegendMin > avail && rows > 3) {
            --rows;
            cw = rows * 2;
        }

        // …and it GROWS when there is room, for the same reason the bar
        // track does. `rows` is a caller's preference, not a maximum: a
        // ring frozen at its 76-column size on a 200-column surface is a
        // small circle adrift in a large void, and the empty space is the
        // widget declining to answer the question it was given room for.
        //
        // The legend keeps its full share throughout — the ring may only
        // grow into space the key does not want, so growing the figure can
        // never cost it its labels.
        //
        // And it may only grow into space the SHEET does not want either.
        // A donut is the one form whose width and height are the same
        // number, so widening it is also lengthening it, and a ring that
        // takes every column on offer takes rows from the tables below it
        // at the same time — which on a narrow surface is how the Cache
        // tab lost its Tokens and Rates sections entirely. `grow_max` is
        // the ceiling the layout imposes from outside; the caller's
        // rows_max is the ceiling the figure imposes on itself, and the
        // smaller of the two wins.
        const int ceiling = std::min(d.rows_max, grow_max > 0 ? grow_max : d.rows_max);
        constexpr int kLegendWant = 22;
        while (rows < ceiling
               && (rows + 1) * 2 + 3 + kLegendWant <= avail) {
            ++rows;
            cw = rows * 2;
        }

        const int legend_w = avail - cw - 3;
        const bool with_legend = legend_w >= kLegendMin;

        if (!d.caption.empty()) {
            out.push_back(Element{TextElement{
                .content = pad + d.caption,
                .style   = Style{}.with_fg(theme.detail),
                .wrap    = TextWrap::TruncateEnd,
            }});
        }

        // Cumulative angles, clockwise from 12 o'clock — the convention
        // every pie chart uses, so a reader's first wedge is where they
        // expect it.
        std::vector<double> edge;
        edge.reserve(d.segments.size() + 1);
        double acc = 0;
        edge.push_back(0.0);
        for (const auto& s : d.segments) {
            acc += (s.value > 0 ? s.value : 0) / total;
            edge.push_back(acc);
        }

        constexpr double kPi = 3.14159265358979323846;
        const int    dot_w = cw * 2;
        const int    dot_h = rows * 4;
        const double cx    = (dot_w - 1) / 2.0;
        const double cy    = (dot_h - 1) / 2.0;
        const double r_out = static_cast<double>(dot_h) / 2.0;
        // A wide hole. The centre text has to clear the inner edge, and a
        // thin ring drawn in dots reads as a ring; a thick one reads as a
        // filled disc with a bite out of it.
        const double r_in  = r_out * 0.62;

        // Which segment owns a dot, or -1 for background.
        auto seg_at = [&](int dx_i, int dy_i) -> int {
            const double dx = dx_i - cx;
            const double dy = dy_i - cy;
            const double r  = std::sqrt(dx * dx + dy * dy);
            if (r > r_out || r < r_in) return -1;
            double a = std::atan2(dx, -dy) / (2.0 * kPi);
            if (a < 0) a += 1.0;
            for (std::size_t i = 0; i + 1 < edge.size(); ++i)
                if (a >= edge[i] && a < edge[i + 1]) return static_cast<int>(i);
            return static_cast<int>(d.segments.size()) - 1;
        };

        // Centre text, laid over the hole.
        auto centre_row = [&](int r) -> const std::string* {
            const int mid = rows / 2;
            if (r == mid && !d.center.empty())         return &d.center;
            if (r == mid + 1 && !d.center_sub.empty()) return &d.center_sub;
            return nullptr;
        };

        std::size_t legend_at = 0;
        for (int cyc = 0; cyc < rows; ++cyc) {
            std::string s = pad;
            std::vector<StyledRun> runs;

            const std::string* ctr = centre_row(cyc);
            const int ctr_w = ctr ? unicode::str_width(*ctr) : 0;
            // Centre the text against the RING, in one division.
            //
            // `cw / 2 - ctr_w / 2` truncates twice and both truncations
            // push the same way, so an odd-width label sat a cell right of
            // the hole it is supposed to be centred in — two columns of air
            // on its left, one on its right, which reads as the ring being
            // lopsided rather than the text being off. `(cw - ctr_w) / 2`
            // is the same intent with one rounding step instead of two.
            const int ctr_x = ctr ? (cw - ctr_w) / 2 : -1;
            // One column of air each side of the centre text. Without it
            // the label butts straight against the inner edge of the ring
            // and the two read as one smear — the hole exists to give the
            // headline somewhere clean to sit.
            const int ctr_lo = ctr ? ctr_x - 1 : -1;
            const int ctr_hi = ctr ? ctr_x + ctr_w + 1 : -1;

            for (int cxc = 0; cxc < cw; ++cxc) {
                if (ctr && cxc >= ctr_lo && cxc < ctr_hi) {
                    if (cxc == ctr_x) {
                        runs.push_back(StyledRun{s.size(), ctr->size(),
                                                 Style{}.with_fg(theme.value)
                                                        .with_bold()});
                        s += *ctr;
                    } else if (cxc < ctr_x || cxc >= ctr_x + ctr_w) {
                        s += ' ';
                    }
                    continue;
                }
                // Gather the cell's 8 dots and vote on its colour. One
                // colour per cell is a braille constraint, and taking the
                // majority is what keeps a wedge boundary from smearing
                // into whichever segment happened to own the first dot.
                std::uint8_t bits = 0;
                int votes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int seg = seg_at(cxc * 2 + dx, cyc * 4 + dy);
                        if (seg < 0) continue;
                        bits |= stat_detail::kBrailleDot[dy][dx];
                        if (seg < 8) ++votes[seg];
                    }
                if (!bits) { s += ' '; continue; }
                int best = 0;
                for (int i = 1; i < 8; ++i) if (votes[i] > votes[best]) best = i;
                const std::string glyph = stat_detail::braille(bits);
                runs.push_back(StyledRun{
                    s.size(), glyph.size(),
                    Style{}.with_fg(
                        d.segments[static_cast<std::size_t>(best)
                                   % d.segments.size()].hue)});
                s += glyph;
            }

            // One legend entry per row, beside the ring. Vertical rather
            // than the band's single line: a ring is tall, and a legend
            // strung under it would put the key further from the wedge it
            // names than the wedge is from its opposite.
            if (with_legend && legend_at < d.segments.size()) {
                const auto& sg = d.segments[legend_at];
                s += "   ";
                const std::string dot{stat_detail::kLegendDot};
                runs.push_back(StyledRun{s.size(), dot.size(),
                                         Style{}.with_fg(sg.hue)});
                s += dot;
                std::string lbl = " " + sg.label;
                const auto fit = unicode::truncate_to_width(lbl, legend_w - 2);
                runs.push_back(StyledRun{s.size(), fit.size(),
                                         Style{}.with_fg(theme.label)});
                s += fit;
                ++legend_at;
            }

            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        }

        // Any legend entries that outran the ring's height get their own
        // rows rather than being dropped — a key that silently omits a
        // wedge is worse than one that costs an extra line.
        while (with_legend && legend_at < d.segments.size()) {
            const auto& sg = d.segments[legend_at++];
            std::string s = pad;
            std::vector<StyledRun> runs;
            s.append(static_cast<std::size_t>(cw) + 3, ' ');
            const std::string dot{stat_detail::kLegendDot};
            runs.push_back(StyledRun{s.size(), dot.size(), Style{}.with_fg(sg.hue)});
            s += dot;
            const std::string lbl = " " + sg.label;
            runs.push_back(StyledRun{s.size(), lbl.size(),
                                     Style{}.with_fg(theme.label)});
            s += lbl;
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        }
    }

    // ── Vertical histogram ───────────────────────────────────────
    //
    // Columns of braille dots rising from a baseline rule, with alternate
    // tick labels beneath. The whole distribution is one shape rather than
    // a list of rows, which is what makes bimodality and skew visible
    // before any label is read.
    static void emit_histogram(const StatHistogram& hg,
                               std::vector<Element>& out,
                               const StatSheetTheme& theme, int avail,
                               const std::string& pad) {
        if (hg.buckets.empty() || avail <= 0) return;
        double hi = 0;
        for (const auto& b : hg.buckets) if (b.value > hi) hi = b.value;
        if (hi <= 0) return;

        const int rows = hg.rows < 2 ? 2 : (hg.rows > 16 ? 16 : hg.rows);

        // The y-axis gutter, reserved before anything is sized — a
        // histogram that overruns its own scale label is worse than one a
        // few columns narrower. Sized to the WIDEST tick so every label
        // right-aligns against the axis; a ragged gutter reads as a
        // second, meaningless column of text.
        int lab_w = 0;
        for (const auto& l : hg.y_labels)
            lab_w = std::max(lab_w, unicode::str_width(l));
        const int gutter = lab_w > 0 ? lab_w + 1 : 0;
        const int plot_w = avail - gutter;
        if (plot_w < 1) return;

        // EVERY bucket is shown. The bar width adapts to the space
        // instead — dropping the tail loses data, and the tail of a
        // latency distribution is exactly where the interesting outliers
        // are. A chart that silently omits its slowest bucket is a chart
        // that answers the wrong question.
        //
        // Widths degrade in order: the requested width, then narrower
        // bars, then bars with no gap between them, and only if even one
        // column per bucket will not fit does it drop from the tail — at
        // which point the surface is too narrow for a chart at all.
        const int want = static_cast<int>(hg.buckets.size());
        int cwid = hg.col_width < 1 ? 1 : hg.col_width;
        if (want > 0) {
            const int afford = plot_w / want;
            if (afford < cwid) cwid = afford;
            // Spare width goes into the BARS, not into trailing blank.
            // `col_width` is a preference like every other figure
            // dimension here; a distribution drawn at 4 columns per bucket
            // on a surface that could afford 9 is throwing away the
            // resolution that makes a shape readable. Capped so a
            // three-bucket histogram does not become three fat slabs.
            else if (afford > cwid) cwid = std::min(afford, hg.col_width_max);
        }
        if (cwid < 1) cwid = 1;
        const int n = std::min(plot_w / cwid, want);
        if (n <= 0) return;
        // Bars keep a gap only while there is width to spare for one. At
        // two columns per bucket the gap is half the chart, so below that
        // the bars run together and the tick labels carry the boundaries.
        const int bar_w = cwid >= 3 ? cwid - 1 : cwid;

        if (!hg.caption.empty()) {
            out.push_back(Element{TextElement{
                .content = pad + hg.caption,
                .style   = Style{}.with_fg(theme.detail),
                .wrap    = TextWrap::TruncateEnd,
            }});
        }

        const Color hue   = hg.hue.value_or(theme.bar);
        const int   dot_h = rows * 4;

        // Column heights in dots. A non-zero bucket always gets at least
        // one dot — the same rule the bars and the band follow, because
        // "almost none" and "none" are different readings.
        std::vector<int> height(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            const double v = hg.buckets[static_cast<std::size_t>(i)].value;
            int d = static_cast<int>(v / hi * dot_h + 0.5);
            if (d == 0 && v > 0) d = 1;
            height[static_cast<std::size_t>(i)] = d > dot_h ? dot_h : d;
        }

        for (int cy = 0; cy < rows; ++cy) {
            // Dot rows this cell row covers, counting DOWN from the top.
            const int top_dot = (rows - 1 - cy) * 4;
            std::string s = pad;
            std::vector<StyledRun> runs;

            // Y-axis tick for this row, right-aligned in the gutter so
            // the numbers line up on their last digit and the axis reads
            // as a scale rather than as ragged prose.
            if (gutter > 0) {
                const auto* tick =
                    cy < static_cast<int>(hg.y_labels.size())
                        ? &hg.y_labels[static_cast<std::size_t>(cy)] : nullptr;
                if (tick && !tick->empty()) {
                    const int tw = unicode::str_width(*tick);
                    s.append(static_cast<std::size_t>(lab_w - tw), ' ');
                    runs.push_back(StyledRun{s.size(), tick->size(),
                                             Style{}.with_fg(theme.detail)});
                    s += *tick;
                    s += ' ';
                } else {
                    s.append(static_cast<std::size_t>(gutter), ' ');
                }
            }

            std::string line;
            for (int i = 0; i < n; ++i) {
                const int h = height[static_cast<std::size_t>(i)];
                std::uint8_t bits = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    // Dot row `top_dot + 3 - dy` measured from the bottom.
                    const int from_bottom = top_dot + (3 - dy);
                    if (from_bottom < h)
                        bits |= stat_detail::kBrailleDot[dy][0]
                              | stat_detail::kBrailleDot[dy][1];
                }
                const std::string glyph = stat_detail::braille(bits);
                // The bar is SOLID across its width bar one column of air.
                // One filled cell followed by two blanks reads as a
                // scatter of dots rather than as a bar chart — the mass of
                // the column is what carries the magnitude.
                //
                // Above the bar the cells are SPACES, not blank braille.
                // U+2800 is a real glyph and many terminals render it a
                // hair differently from a space, which paints a faint
                // rectangle over the whole plot area — visible as a box
                // around the chart that nobody asked for.
                for (int k = 0; k < bar_w; ++k) {
                    if (bits) line += glyph;
                    else      line += ' ';
                }
                for (int k = bar_w; k < cwid; ++k) line += ' ';
            }
            runs.push_back(StyledRun{s.size(), line.size(),
                                     Style{}.with_fg(hue)});
            s += line;
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        }

        // Baseline. A histogram floating with no axis has no zero, and
        // "short bar" then means nothing in particular.
        {
            std::string s = pad;
            if (gutter > 0) s.append(static_cast<std::size_t>(gutter), ' ');
            std::string rule;
            for (int i = 0; i < n * cwid; ++i) rule += "\xe2\x94\x80";   // ─
            std::vector<StyledRun> runs{
                StyledRun{s.size(), rule.size(), Style{}.with_fg(theme.track)}};
            s += rule;
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        }

        // Tick labels. Stride is chosen so consecutive labels cannot
        // collide: a label needs its own width plus a space, and at
        // narrow bar widths that means naming every third or fourth
        // bucket rather than every second. Computing it from the widest
        // label — rather than hard-coding "every other one" — is what
        // keeps the axis readable when the bars shrink to fit.
        bool any_label = false;
        int  widest_lb = 0;
        for (int i = 0; i < n; ++i) {
            const auto& lb = hg.buckets[static_cast<std::size_t>(i)].label;
            if (!lb.empty()) {
                any_label = true;
                widest_lb = std::max(widest_lb, unicode::str_width(lb));
            }
        }
        if (!any_label) return;
        {
            int stride = 1;
            while (stride * cwid < widest_lb + 1 && stride < n) ++stride;
            std::string s = pad;
            if (gutter > 0) s.append(static_cast<std::size_t>(gutter), ' ');
            std::vector<StyledRun> runs;
            int col = 0;
            for (int i = 0; i < n; i += stride) {
                const auto& lb = hg.buckets[static_cast<std::size_t>(i)].label;
                if (lb.empty()) continue;
                const int want = i * cwid;
                if (want < col) continue;          // previous label still running
                s.append(static_cast<std::size_t>(want - col), ' ');
                col = want;
                const auto fit_lb =
                    unicode::truncate_to_width(lb, stride * cwid - 1);
                runs.push_back(StyledRun{s.size(), fit_lb.size(),
                                         Style{}.with_fg(theme.detail)});
                s += fit_lb;
                col += unicode::str_width(fit_lb);
            }
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        }
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
            // Area, in BRAILLE with a stippled fill.
            //
            // The curve itself is solid dots; the region under it is filled
            // with every other dot, so it reads as a shaded area with the
            // line still legible on top of it. Filling solid was the first
            // attempt and it produced a slab of ⠳⠳⠳ where the surface — the
            // one thing a reader is looking for — disappeared into the mass.
            //
            // Braille rather than blocks because a block column has a flat
            // top edge one eighth of a row tall, which quantises a smooth
            // series into a staircase. At 2x4 dots the curve keeps its
            // shape and the fill stays subordinate to it.
            const int dot_w = cells * 2;
            const int dot_h = rows  * 4;
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
                // Resample by MAX over the samples this dot column covers.
                // When 40 turns share 130 dot columns a spike that survives
                // to the screen is the honest reduction; a mean would erase
                // exactly the outlier the reader is looking for.
                const std::size_t lo = n * static_cast<std::size_t>(x)
                                     / static_cast<std::size_t>(dot_w);
                std::size_t hi_i = n * static_cast<std::size_t>(x + 1)
                                 / static_cast<std::size_t>(dot_w);
                if (hi_i <= lo) hi_i = lo + 1;
                double peak = 0;
                for (std::size_t i = lo; i < hi_i && i < n; ++i)
                    if (p.series[i] > peak) peak = p.series[i];
                int y = dot_h - 1 - static_cast<int>(peak / hi * (dot_h - 1) + 0.5);
                if (y < 0) y = 0;
                if (y >= dot_h) y = dot_h - 1;
                return y;
            };
            int prev_y = y_at(0);
            for (int x = 0; x < dot_w; ++x) {
                const int y = y_at(x);
                // The curve: solid, including the vertical stroke that
                // joins consecutive samples. Without the join a steep move
                // leaves a gap and the eye reads two unrelated marks
                // instead of one falling line.
                const int lo = y < prev_y ? y : prev_y;
                const int hi_y = y < prev_y ? prev_y : y;
                for (int yy = lo; yy <= hi_y; ++yy) dot_at(x, yy);
                // The fill: a sparse stipple below the curve — one dot in
                // four, on even columns and even rows. At one-in-two the
                // texture was dense enough to compete with the line, and
                // the whole point of stippling rather than filling solid
                // is that the SURFACE stays the thing you see. One in four
                // reads as shading; the curve reads as the curve.
                for (int yy = hi_y + 1; yy < dot_h; ++yy)
                    if ((x % 2) == 0 && (yy % 2) == 0) dot_at(x, yy);
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
    int track_     = 14;
    int indent_    = 0;
    int reserve_   = 0;
    int col_min_w_ = 0;
    int budget_    = 0;
    // One column unless a host opts in. Splitting changes reading order,
    // so it is a decision a host makes, never a default it inherits.
    int col_max_   = 1;
};

}  // namespace maya
