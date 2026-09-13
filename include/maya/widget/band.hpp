#pragma once
// maya::Band — a composition drawn as one full-width bar with a legend
// beneath it.
//
// Takes two lines: the bar, then the key that says what the segments are. An
// unlabelled band is a few coloured lengths and no information, so the legend
// travels with it.
//
// Usage:
//   Band b;
//   b.segment("cache 60%", 60, Color::green())
//    .segment("miss 40%",  40, Color::red())
//    .caption("requests");
//   Element ui = b.build();
//
// Standalone: sizes the bar to whatever width the layout gives it (via
// fill()).

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"
#include "../text/unicode_width.hpp"

namespace maya {

class Band {
public:
    struct Seg {
        std::string label;
        double      value = 0;
        Color       hue;
    };

    Band() = default;
    explicit Band(std::vector<Seg> segs) : segments_(std::move(segs)) {}

    Band& segment(std::string label, double value, Color hue) {
        segments_.push_back({std::move(label), value, hue});
        return *this;
    }
    Band& caption(std::string c)   { caption_ = std::move(c); return *this; }
    Band& label_color(Color c)     { label_color_ = c; return *this; }

    operator Element() const { return build(); }

    // Two lines (bar + legend), plus the caption when there is one.
    [[nodiscard]] int height() const {
        if (segments_.empty()) return 1;
        return caption_.empty() ? 2 : 3;
    }

    [[nodiscard]] Element build() const {
        double total = 0;
        for (const auto& s : segments_) total += s.value > 0 ? s.value : 0;
        if (segments_.empty() || total <= 0)
            return Element{TextElement{}};

        std::vector<Element> lines;

        if (!caption_.empty())
            lines.push_back(Element{TextElement{
                .content = caption_,
                .style   = Style{}.with_fg(label_color_),
                .wrap    = TextWrap::TruncateEnd}});

        // The bar fills whatever width the engine settles on, so this kind has
        // no width arithmetic of its own — the reason a picture belongs in the
        // layout rather than in a hand-built string.
        auto segs = segments_;
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
            for (const auto& seg : segments_) {
                if (!s.empty()) s += "   ";
                const std::size_t at = s.size();
                s += "\xe2\x96\xa0 ";                                   // ■
                runs.push_back({at, s.size() - at, Style{}.with_fg(seg.hue)});
                // The label is the WHOLE legend entry; callers format their
                // own share into it.
                const std::size_t tat = s.size();
                s += seg.label;
                runs.push_back({tat, s.size() - tat,
                                Style{}.with_fg(label_color_)});
            }
            lines.push_back(Element{TextElement{.content = std::move(s),
                                                .wrap    = TextWrap::TruncateEnd,
                                                .runs    = std::move(runs)}});
        }

        return dsl::v(std::move(lines)).build();
    }

private:
    std::vector<Seg> segments_;
    std::string      caption_;    // a dim note above the bar; empty = none
    Color            label_color_ = Color::bright_black();
};

} // namespace maya
