#pragma once
// maya::widget::gradient — Apply color gradients to text
//
// Usage:
//   auto elem = gradient("Hello World", Color::red(), Color::blue());
//
// Each character is tinted with a true 24-bit RGB blend between the stops
// (via Color::to_rgb(), so named/indexed stops resolve to real channels
// first). The terminal-capability degrade pass downgrades the resulting
// truecolor to 256/16 as needed, so this looks right everywhere.

#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <cmath>

namespace maya {

namespace detail {

// Linear RGB blend between two colors at parameter t∈[0,1]. Both stops are
// resolved to true channels first so named/indexed stops interpolate too.
[[nodiscard]] inline Color lerp_color(Color from, Color to, float t) noexcept {
    Color a = from.to_rgb(), b = to.to_rgb();
    auto mix = [t](std::uint8_t x, std::uint8_t y) -> std::uint8_t {
        float v = static_cast<float>(x) + (static_cast<float>(y) - x) * t;
        return static_cast<std::uint8_t>(v + 0.5f);
    };
    return Color::rgb(mix(a.r(), b.r()), mix(a.g(), b.g()), mix(a.b(), b.b()));
}

} // namespace detail

/// Build a TextElement with a linear color gradient across characters.
/// Interpolates between `from` and `to` colors per-character.
[[nodiscard]] inline Element gradient(std::string_view text,
                                       Color from, Color to,
                                       bool bold = false) {
    if (text.empty()) return Element{TextElement{}};

    // Count characters (not bytes) for gradient steps
    std::vector<std::pair<std::size_t, std::size_t>> char_spans; // byte_offset, byte_len
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = pos;
        (void)decode_utf8(text, pos);
        char_spans.push_back({start, pos - start});
    }

    int n = static_cast<int>(char_spans.size());
    if (n <= 1) {
        Style s = Style{}.with_fg(from);
        if (bold) s = s.with_bold();
        return Element{TextElement{
            .content = std::string{text},
            .style = s,
        }};
    }

    std::vector<StyledRun> runs;
    runs.reserve(char_spans.size());

    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n - 1);
        Style s = Style{}.with_fg(detail::lerp_color(from, to, t));
        if (bold) s = s.with_bold();
        runs.push_back({char_spans[i].first, char_spans[i].second, s});
    }

    return Element{TextElement{
        .content = std::string{text},
        .style = Style{}.with_fg(from),
        .runs = std::move(runs),
    }};
}

/// Multi-stop gradient. Colors are evenly distributed across the text.
[[nodiscard]] inline Element gradient(std::string_view text,
                                       std::initializer_list<Color> stops,
                                       bool bold = false) {
    if (stops.size() < 2) {
        if (stops.size() == 1) {
            Style s = Style{}.with_fg(*stops.begin());
            if (bold) s = s.with_bold();
            return Element{TextElement{.content = std::string{text}, .style = s}};
        }
        return Element{TextElement{.content = std::string{text}}};
    }

    // Count characters
    std::vector<std::pair<std::size_t, std::size_t>> char_spans;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = pos;
        (void)decode_utf8(text, pos);
        char_spans.push_back({start, pos - start});
    }

    int n = static_cast<int>(char_spans.size());
    if (n <= 1) {
        Style s = Style{}.with_fg(*stops.begin());
        if (bold) s = s.with_bold();
        return Element{TextElement{.content = std::string{text}, .style = s}};
    }

    auto colors = std::vector<Color>(stops);
    int segments = static_cast<int>(colors.size()) - 1;

    std::vector<StyledRun> runs;
    runs.reserve(char_spans.size());

    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n - 1);
        float segment_f = t * static_cast<float>(segments);
        int seg = std::min(static_cast<int>(segment_f), segments - 1);
        float local_t = segment_f - static_cast<float>(seg);

        Style s = Style{}.with_fg(
            detail::lerp_color(colors[seg], colors[seg + 1], local_t));
        if (bold) s = s.with_bold();
        runs.push_back({char_spans[i].first, char_spans[i].second, s});
    }

    return Element{TextElement{
        .content = std::string{text},
        .style = Style{}.with_fg(*stops.begin()),
        .runs = std::move(runs),
    }};
}

} // namespace maya
