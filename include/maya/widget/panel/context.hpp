#pragma once
// maya::panel::ItemCtx — what an item widget may know while rendering.
//
// Deliberately tiny. An item renders from its own value plus this context,
// and NOTHING else — no Config, no row list, no layout. That is what keeps
// "one item kind = one widget file" honest: a widget that could reach the
// whole panel would grow panel logic.

#include "theme.hpp"

namespace maya::panel {

struct ItemCtx {
    const Theme& theme;

    // Choice only: is this row's dropdown currently open?
    bool open = false;

    // Text/Path: columns the edited value may occupy before it scrolls
    // horizontally under its caret (Panel::kEditBudget). 0 = unmeasured.
    int edit_budget = 0;
};

} // namespace maya::panel
