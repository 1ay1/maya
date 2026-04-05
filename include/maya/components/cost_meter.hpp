#pragma once
// maya::components::CostMeter — AI token usage and cost display
//
//   CostMeter({.input_tokens = 1234, .output_tokens = 340,
//              .cost = 0.042, .model = "claude-opus-4-6"})
//
//   CostMeter({.input_tokens = 5000, .output_tokens = 1200,
//              .cost = 0.42, .budget = 5.0, .compact = true})

#include "core.hpp"

namespace maya::components {

struct CostMeterProps {
    int         input_tokens  = 0;
    int         output_tokens = 0;
    int         cache_read    = 0;
    int         cache_write   = 0;
    double      cost          = 0.0;
    double      budget        = 0.0;
    std::string model         = "";
    bool        compact       = false;
};

namespace detail {

std::string format_tokens(int n);

} // namespace detail

Element CostMeter(CostMeterProps props = {});

} // namespace maya::components
