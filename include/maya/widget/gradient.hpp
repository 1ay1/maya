#pragma once
// maya::widget::gradient — Apply color gradients to text
//
// Usage:
//   auto elem = gradient("Hello World", Color::rgb(255, 0, 0), Color::rgb(0, 0, 255));

#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <cmath>

namespace maya {

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
        uint8_t r = static_cast<uint8_t>(from.r() * (1.0f - t) + to.r() * t);
        uint8_t g = static_cast<uint8_t>(from.g() * (1.0f - t) + to.g() * t);
        uint8_t b = static_cast<uint8_t>(from.b() * (1.0f - t) + to.b() * t);
        Style s = Style{}.with_fg(Color::rgb(r, g, b));
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

        auto& c0 = colors[seg];
        auto& c1 = colors[seg + 1];
        uint8_t r = static_cast<uint8_t>(c0.r() * (1.0f - local_t) + c1.r() * local_t);
        uint8_t g = static_cast<uint8_t>(c0.g() * (1.0f - local_t) + c1.g() * local_t);
        uint8_t b = static_cast<uint8_t>(c0.b() * (1.0f - local_t) + c1.b() * local_t);
        Style s = Style{}.with_fg(Color::rgb(r, g, b));
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
