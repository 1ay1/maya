#pragma once
// maya::panel item widget: Band — a composition, drawn as one full-width bar
// with a legend beneath it.
//
// The first PICTURE kind. Meter and Spark are strips that share a row with a
// label and a value; a Band is the content of its row, and it takes two
// lines: the bar, then the key that says what the segments are.
//
// Two lines is why it needs rows_of(): item_lines() decides a row's height
// without painting it, so a control that paints more than one line has to
// say so. Before that existed, every control was one line by assumption,
// and a picture had nowhere to live inside a panel.
//
// An unlabelled band is a few coloured lengths and no information, so the
// legend is not optional — when the surface cannot hold both, the LABELS
// shorten before the key disappears.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../../../dsl.hpp"
#include "../../../element/element.hpp"
#include "../../../element/text.hpp"
#include "../../../style/color.hpp"
#include "../../../style/style.hpp"
#include "../../../text/unicode_width.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Band {
    struct Seg {
        std::string label;
        double      value = 0;
        Color       hue;
    };
    std::vector<Seg> segments;
    // A dim note above the bar. Empty = no caption row.
    std::string caption;
};

// Two lines: the bar and its legend. Plus the caption when there is one.
[[nodiscard]] inline int rows_of(const Band& b, const ItemCtx&) {
    if (b.segments.empty()) return 1;
    return b.caption.empty() ? 2 : 3;
}

[[nodiscard]] inline std::pair<std::string, Style>
render(const Band& b, const ItemCtx& ctx) {
    // The string path exists so every kind answers render(); a Band is a
    // picture and answers properly through render_element below.
    (void)b;
    return {std::string{}, Style{}.with_fg(ctx.theme.value)};
}

[[nodiscard]] inline Element render_element(const Band& b, const ItemCtx& ctx) {
    double total = 0;
    for (const auto& s : b.segments) total += s.value > 0 ? s.value : 0;
    if (b.segments.empty() || total <= 0)
        return Element{TextElement{}};

    std::vector<Element> lines;
    if (!b.caption.empty())
        lines.push_back(Element{TextElement{
            .content = b.caption,
            .style   = Style{}.with_fg(ctx.theme.help),
            .wrap    = TextWrap::TruncateEnd}});

    // The bar fills whatever width the engine settles on, so this kind has
    // no width arithmetic of its own — the reason a picture belongs in the
    // layout rather than in a hand-built string.
    auto segs = b.segments;
    lines.push_back(dsl::fill([segs, total](int w, int) {
        const int cells = w > 0 ? w : 0;

        // Largest-remainder apportionment, not independent rounding.
        // Rounding each segment on its own leaves the total a column or
        // two short and the bar's right edge ragged against its frame.
        std::vector<int>    cols(segs.size(), 0);
        std::vector<double> rem(segs.size(), 0.0);
        int used = 0;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const double exact = (segs[i].value > 0 ? segs[i].value : 0)
                               / total * cells;
            cols[i] = static_cast<int>(exact);
            rem[i]  = exact - cols[i];
            used   += cols[i];
        }
        while (used < cells) {
            std::size_t best = 0;
            double best_r = -1.0;
            for (std::size_t i = 0; i < rem.size(); ++i)
                if (rem[i] > best_r) { best_r = rem[i]; best = i; }
            if (best_r < 0) break;
            ++cols[best]; rem[best] = -1.0; ++used;
        }

        std::string s;
        std::vector<StyledRun> runs;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (cols[i] <= 0) continue;
            const std::size_t at = s.size();
            for (int c = 0; c < cols[i]; ++c) s += "\xe2\x96\x88";   // █
            runs.push_back({at, s.size() - at, Style{}.with_fg(segs[i].hue)});
        }
        return Element{TextElement{.content = std::move(s),
                                   .wrap    = TextWrap::TruncateEnd,
                                   .runs    = std::move(runs)}};
    }).build());

    // The legend: a swatch, a name and a share, per segment.
    {
        std::string s;
        std::vector<StyledRun> runs;
        for (const auto& seg : b.segments) {
            if (!s.empty()) s += "   ";
            const std::size_t at = s.size();
            s += "\xe2\x96\xa0 ";                                   // ■
            runs.push_back({at, s.size() - at, Style{}.with_fg(seg.hue)});
            const int pct = static_cast<int>(
                (seg.value > 0 ? seg.value : 0) / total * 100.0 + 0.5);
            const std::size_t tat = s.size();
            s += seg.label + " " + std::to_string(pct) + "%";
            runs.push_back({tat, s.size() - tat,
                            Style{}.with_fg(ctx.theme.help)});
        }
        lines.push_back(Element{TextElement{.content = std::move(s),
                                            .wrap    = TextWrap::TruncateEnd,
                                            .runs    = std::move(runs)}});
    }

    return dsl::v(std::move(lines)).build();
}

} // namespace maya::panel
