#pragma once
// maya::components::GlimmerText — Shimmer streaming animation
//
//   GlimmerText({.text = partial_response, .frame = tick, .is_streaming = true})
//   GlimmerText({.text = final_response, .is_streaming = false})

#include "core.hpp"

namespace maya::components {

struct GlimmerTextProps {
    std::string text_content;
    int         frame         = 0;
    bool        is_streaming  = true;
    int         wave_width    = 8;
    Color       base_color    = palette().text;
    Color       shimmer_start = Color::rgb(230, 160, 80);   // warm orange
    Color       shimmer_mid   = Color::rgb(255, 210, 100);  // gold
    Color       shimmer_end   = Color::rgb(255, 255, 220);  // white
};

namespace detail {

Color lerp_color(Color a, Color b, float t);


Color shimmer_color(float t, Color start, Color mid, Color end);


} // namespace detail

Element GlimmerText(GlimmerTextProps props);


} // namespace maya::components
