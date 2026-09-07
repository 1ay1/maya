#pragma once
// maya::panel item widget: Number — a bounded integer, edited digit-wise.

#include <cstdint>
#include <string>
#include <utility>

#include "../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Number { std::int64_t value = 0; };

[[nodiscard]] inline std::pair<std::string, Style>
render(const Number& c, const ItemCtx& ctx) {
    return {std::to_string(c.value), Style{}.with_fg(ctx.theme.value)};
}

} // namespace maya::panel
