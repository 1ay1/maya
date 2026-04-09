#pragma once
// maya::platform::win32::Win32ResizeSignal - Console resize detection
//
// On Windows, resize events are detected by polling the console buffer
// size (integrated into Win32EventSource). This class is a lightweight
// no-op that satisfies the ResizeSource concept — the real resize
// detection happens in Win32EventSource::wait().

#if !MAYA_PLATFORM_WIN32
#error "This header is for Windows only"
#endif

#include "../../core/expected.hpp"
#include "../io.hpp"

namespace maya::platform::win32 {

// ============================================================================
// Win32ResizeSignal — no-op (resize detected by EventSource polling)
// ============================================================================

class Win32ResizeSignal {
    Win32ResizeSignal() = default;

public:
    [[nodiscard]] static auto install() -> Result<Win32ResizeSignal> {
        return ok(Win32ResizeSignal{});
    }

    [[nodiscard]] bool pending() const { return false; }
    void drain() {}
    [[nodiscard]] NativeHandle native_handle() const noexcept { return invalid_handle; }

    // -- Move-only ------------------------------------------------------------

    Win32ResizeSignal(Win32ResizeSignal&&) noexcept = default;
    Win32ResizeSignal& operator=(Win32ResizeSignal&&) noexcept = default;
    Win32ResizeSignal(const Win32ResizeSignal&) = delete;
    Win32ResizeSignal& operator=(const Win32ResizeSignal&) = delete;
};

} // namespace maya::platform::win32
