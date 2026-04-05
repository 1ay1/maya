#pragma once
// maya::components::TokenStream — Live token generation rate visualization
//
//   TokenStream({.total_tokens = 3421, .tokens_per_sec = 82.3f,
//                .peak_rate = 94.1f, .elapsed_secs = 12.4f,
//                .rate_history = history_vec})
//
//   TokenStream({.total_tokens = 500, .tokens_per_sec = 42.0f,
//                .rate_history = history_vec, .compact = true})

#include "core.hpp"

namespace maya::components {

struct TokenStreamProps {
    int                total_tokens  = 0;       // total generated so far
    float              tokens_per_sec = 0.f;    // current rate
    float              peak_rate     = 0.f;     // peak observed rate
    float              elapsed_secs  = 0.f;     // time since start
    std::vector<float> rate_history  = {};       // historical tok/s values for sparkline
    Color              color         = {};       // default: palette().primary
    bool               compact       = false;   // single-line mode
};

namespace detail {

std::string format_with_commas(int n);
Color rate_color(float rate);
Color resolve_color(Color c);

} // namespace detail

Element TokenStream(TokenStreamProps props = {});

} // namespace maya::components
