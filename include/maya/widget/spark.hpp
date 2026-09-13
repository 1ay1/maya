#pragma once
// maya::Spark — a series drawn as a one-line strip.
//
// One cell per SAMPLE — its natural width is the data, and a budget is a
// WINDOW onto it, not a size to stretch to. (That is what separates it from
// a Meter, which fills its budget.)
//
// Eight levels (▁▂▃▄▅▆▇█) rather than braille: a strip that sits inline in a
// table row shares its baseline with text, and braille's 2×4 grid reads as a
// different typeface at that size.
//
// NOTE: this is distinct from maya::Sparkline — that is a different widget.
//
// Usage:
//   Spark s;
//   s.series({1,4,2,6,3,8,5}).value("8");
//   Element ui = s.build();
//
// Standalone: shows the most recent `width` samples (default 24, clamped
// 4..40) and lays the value beside the strip as a sibling.

#include <algorithm>
#include <optional>
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

class Spark {
public:
    Spark() = default;
    explicit Spark(std::vector<double> s) : series_(std::move(s)) {}

    Spark& series(std::vector<double> s) { series_ = std::move(s); return *this; }
    Spark& push(double v)                { series_.push_back(v); return *this; }
    Spark& value(std::string v)          { value_ = std::move(v); return *this; }
    Spark& width(int cells)              { width_ = cells; return *this; }
    Spark& line_color(Color c)           { line_color_ = c; return *this; }
    Spark& value_color(Color c)          { value_color_ = c; return *this; }

    // Kept so callers that read the raw value (as panel did via `.value`)
    // still work.
    [[nodiscard]] const std::string& value() const { return value_; }

    operator Element() const { return build(); }

    // The strip string alone, without the value cell.
    [[nodiscard]] std::string strip() const {
        if (series_.empty()) return std::string{};

        // The budget is a WINDOW, not a size. The strip is as wide as its
        // data or as wide as the budget, whichever is smaller.
        const int room = std::clamp(width_, 4, 40);
        const std::size_t take =
            series_.size() > static_cast<std::size_t>(room)
                ? static_cast<std::size_t>(room) : series_.size();

        // Show the most RECENT samples — "what is happening" is about the tail.
        const std::size_t from = series_.size() - take;

        double hi = 0.0;
        double lo = 0.0;
        bool first = true;
        for (std::size_t i = from; i < series_.size(); ++i) {
            const double v = series_[i];
            if (first) { hi = lo = v; first = false; }
            else { hi = std::max(hi, v); lo = std::min(lo, v); }
        }

        static constexpr const char* kLevels[8] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
        };

        // A FLAT series has no shape, and drawing one would be inventing a
        // reading. Tested against the span, because a series flat at 5 and one
        // flat at 5000 are equally shapeless.
        const bool flat = (hi - lo) <= 0.0;

        std::string out;
        for (std::size_t i = from; i < series_.size(); ++i) {
            // Scaled against the window's MAX, not its span: rescaling 90..100
            // to fill the full height would make a 10% wobble look like a
            // collapse.
            const int lvl = flat
                ? 0
                : std::clamp(static_cast<int>(series_[i] / hi * 7.0 + 0.5), 0, 7);
            out += kLevels[lvl];
        }
        return out;
    }

    [[nodiscard]] Element build() const {
        const Style style = Style{}.with_fg(line_color_);
        const std::string s = strip();

        Element bar{TextElement{.content = s,
                                .style   = style,
                                .wrap    = TextWrap::TruncateEnd}};
        if (value_.empty()) return bar;

        const int vw = static_cast<int>(unicode::str_width(value_));
        Element val{TextElement{.content = value_,
                                .style   = Style{}.with_fg(value_color_),
                                .wrap    = TextWrap::TruncateEnd}};

        // A spacer between them takes the slack, so the strip stays its natural
        // width and the value lands at the far end.
        return dsl::h(std::move(bar) | dsl::shrink(0.0f),
                      dsl::spacer().build() | dsl::grow(1.0f),
                      std::move(val) | dsl::width(vw)
                                     | dsl::shrink(0.0f)
                                     | dsl::justify(Justify::End))
               | dsl::gap(1);
    }

private:
    std::vector<double> series_;
    std::string         value_;               // the number the strip annotates
    int                 width_ = 24;          // window width (clamped 4..40)
    Color               line_color_  = Color::hex(0xCDD6F4);
    Color               value_color_ = Color::hex(0xCDD6F4);
};

} // namespace maya
