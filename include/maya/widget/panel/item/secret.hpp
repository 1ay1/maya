#pragma once
// maya::panel item widget: Secret — a credential.
//
// No plaintext — by construction. The value is a CHARACTER COUNT, not a
// string, so no render path (present or future) can leak a credential to the
// screen: there is nothing to leak. Masking is type-level, not a flag someone
// can forget to check.

#include <algorithm>
#include <string>
#include <utility>

#include "../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Secret {
    std::size_t filled = 0;
    std::size_t caret  = std::string::npos;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Secret& c, const ItemCtx& ctx) {
    // Capped, so the rendered width never discloses the real length.
    const bool editing = c.caret != std::string::npos;
    if (c.filled == 0)
        return {editing ? "\xe2\x96\x88 empty" : "not set",
                Style{}.with_fg(editing ? ctx.theme.value_edit : ctx.theme.off)};
    std::string dots(std::min<std::size_t>(c.filled, 12), '*');
    if (editing) dots += "\xe2\x96\x88";
    return {dots, Style{}.with_fg(editing ? ctx.theme.value_edit
                                          : ctx.theme.value)};
}

} // namespace maya::panel
