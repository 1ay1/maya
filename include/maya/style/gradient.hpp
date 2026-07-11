#pragma once
// maya::Gradient — smooth multi-stop color interpolation.
//
// A Gradient is an ordered list of color stops sampled by a normalized
// parameter t ∈ [0, 1]. at(0) is the first stop, at(1) the last, and any
// value between is a linear RGB blend across the two nearest stops. It is
// the shared color engine behind gradient() text, gradient_rule(), and any
// hand-rolled fade you want to drive per-cell.
//
//   Gradient g{{ Color::hex(0xFF5F6D), Color::hex(0xFFC371) }};
//   Color mid = g.at(0.5f);                 // a warm orange
//
//   // sweep a value bar through a health palette:
//   Gradient health{{ Color::hex(0x2ECC71),   // green
//                     Color::hex(0xF1C40F),   // amber
//                     Color::hex(0xE74C3C) }}; // red
//   Color c = health.at(load /* 0..1 */);
//
// Stops of ANY Color::Kind work — Named / Indexed are resolved to true RGB
// via Color::to_rgb() before blending, so the interpolation is always
// perceptually sensible (the raw r()/g()/b() accessors would mix a palette
// index as if it were a red channel). Pure arithmetic, constexpr-friendly,
// no dependencies beyond the color type.

#include "color.hpp"

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace maya {

struct Gradient {
    std::vector<Color> stops;

    Gradient() = default;
    Gradient(std::initializer_list<Color> s) : stops(s) {}
    explicit Gradient(std::vector<Color> s) : stops(std::move(s)) {}

    /// Two-stop convenience factory: from → to.
    [[nodiscard]] static Gradient two(Color from, Color to) {
        return Gradient{from, to};
    }

    /// Is there anything to sample?
    [[nodiscard]] bool empty() const noexcept { return stops.empty(); }

    /// Sample the gradient at t ∈ [0, 1] (clamped). Blends the two nearest
    /// stops in RGB space. Empty → white; single stop → that stop.
    [[nodiscard]] Color at(float t) const noexcept {
        if (stops.empty()) return Color::rgb(255, 255, 255);
        if (stops.size() == 1) return stops.front().to_rgb();

        t = std::clamp(t, 0.0f, 1.0f);
        const float scaled = t * static_cast<float>(stops.size() - 1);
        auto  i = static_cast<std::size_t>(scaled);
        if (i >= stops.size() - 1) return stops.back().to_rgb();
        const float frac = scaled - static_cast<float>(i);

        const Color a = stops[i].to_rgb();
        const Color b = stops[i + 1].to_rgb();
        auto mix = [frac](uint8_t x, uint8_t y) -> uint8_t {
            const float v = static_cast<float>(x) +
                            (static_cast<float>(y) - static_cast<float>(x)) * frac;
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
        };
        return Color::rgb(mix(a.r(), b.r()), mix(a.g(), b.g()), mix(a.b(), b.b()));
    }
};

} // namespace maya
