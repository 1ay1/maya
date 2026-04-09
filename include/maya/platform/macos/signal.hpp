#pragma once
// maya::platform::macos::MacResizeSignal - kqueue-integrated resize detection
//
// On macOS, SIGWINCH is handled directly by kqueue via EVFILT_SIGNAL.
// No self-pipe trick needed. This class is a minimal shim that satisfies
// the ResizeSource concept — the real work happens in MacEventSource.

#if !MAYA_PLATFORM_MACOS
#error "This header is for macOS only"
#endif

#include <utility>

#include "../../core/expected.hpp"
#include "../io.hpp"

namespace maya::platform::macos {

// ============================================================================
// MacResizeSignal — no-op (kqueue handles SIGWINCH directly)
// ============================================================================

class MacResizeSignal {
    MacResizeSignal() = default;

public:
    [[nodiscard]] static auto install() -> Result<MacResizeSignal> {
        // Nothing to install — MacEventSource registers EVFILT_SIGNAL
        return ok(MacResizeSignal{});
    }

    [[nodiscard]] bool pending() const { return false; }

    void drain() {
        // kqueue drains signal events automatically when kevent() returns them
    }

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return invalid_handle;
    }

    // -- Move-only ------------------------------------------------------------

    MacResizeSignal(MacResizeSignal&&) noexcept = default;
    MacResizeSignal& operator=(MacResizeSignal&&) noexcept = default;
    MacResizeSignal(const MacResizeSignal&) = delete;
    MacResizeSignal& operator=(const MacResizeSignal&) = delete;
    ~MacResizeSignal() = default;
};

} // namespace maya::platform::macos
