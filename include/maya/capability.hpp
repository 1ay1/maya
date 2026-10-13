#pragma once
// maya/capability.hpp — the lock-order capability types alone.
//
// A header that only needs to NAME `const maya::unlocked&` (an interface
// that passes a capability through) shouldn't drag in the whole runtime
// (<maya/runtime.hpp> pulls the kernel, the reactors and the platform
// layer). These are the same aliases runtime.hpp declares — including both
// is fine; a using-declaration may be repeated.
//
// An app still never spells jaal: this file is the seam (agentty → maya →
// jaal, docs/LAYERING.md).

#include <jaal/core/capability.hpp>

namespace maya {
using ::jaal::unlocked;   // const unlocked&: holds nothing (may block, may take any lock)
using ::jaal::held;       // const held<R>&: inside a lock ranked R
using ::jaal::below;      // const below<R>&: caller holds only ranks < R
using ::jaal::rank;
using ::jaal::leaf;
using ::jaal::Rank;
}  // namespace maya
