#pragma once
// maya::platform::win32::Win32EventSource - Console input event multiplexer
//
// Properly classifies console events in the input queue:
//   - WINDOW_BUFFER_SIZE_EVENT → resize flag (consumed, never reaches ReadFile)
//   - KEY_EVENT / MOUSE_EVENT  → input flag  (left for ReadFile to translate)
//   - FOCUS_EVENT / MENU_EVENT → discarded   (noise on Windows consoles)
//
// ReadFile in VT mode can only translate KEY_EVENT to byte sequences.
// System events (resize, focus, menu) are opaque to ReadFile and will
// block it indefinitely if left in the queue. This class drains them.

#if !MAYA_PLATFORM_WIN32
#error "This header is for Windows only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <chrono>

#include "../../core/expected.hpp"
#include "../concepts.hpp"
#include "../io.hpp"

namespace maya::platform::win32 {

// ============================================================================
// Win32EventSource — console event-aware multiplexer
// ============================================================================

class Win32EventSource {
    HANDLE stdin_;
    HANDLE stdout_;
    HANDLE wake_ = INVALID_HANDLE_VALUE;

public:
    Win32EventSource(NativeHandle term_in, [[maybe_unused]] NativeHandle sig_handle) noexcept
        : stdin_(term_in)
        , stdout_(::GetStdHandle(STD_OUTPUT_HANDLE))
    {}

    void set_wake_handle(HANDLE h) noexcept { wake_ = h; }

    [[nodiscard]] auto wait(
        std::chrono::milliseconds timeout,
        [[maybe_unused]] bool want_write = false) -> Result<ReadyFlags>
    {
        ReadyFlags flags{};
        flags.writeable = true;

        DWORD ms = static_cast<DWORD>(timeout.count());

        if (wake_ != INVALID_HANDLE_VALUE) {
            HANDLE handles[2] = { stdin_, wake_ };
            DWORD result = ::WaitForMultipleObjects(2, handles, FALSE, ms);
            if (result == WAIT_OBJECT_0) {
                drain_system_events(flags);
            } else if (result == WAIT_OBJECT_0 + 1) {
                flags.wake = true;
            }
        } else {
            DWORD result = ::WaitForSingleObject(stdin_, ms);
            if (result == WAIT_OBJECT_0) {
                drain_system_events(flags);
            }
        }

        return ok(ReadyFlags{flags});
    }

    // -- Move-only (no resources to manage) -----------------------------------

    Win32EventSource(Win32EventSource&&) noexcept = default;
    Win32EventSource& operator=(Win32EventSource&&) noexcept = default;
    Win32EventSource(const Win32EventSource&) = delete;
    Win32EventSource& operator=(const Win32EventSource&) = delete;

private:
    // Peek at the front of the console input queue. Consume events that
    // ReadFile cannot translate in VT mode (resize, focus, menu) so they
    // don't block the byte-oriented read path. Stop at the first real
    // input event (KEY_EVENT, MOUSE_EVENT) — those stay for ReadFile.
    void drain_system_events(ReadyFlags& flags) noexcept {
        INPUT_RECORD rec;
        DWORD n;
        while (::PeekConsoleInputW(stdin_, &rec, 1, &n) && n > 0) {
            switch (rec.EventType) {
            case WINDOW_BUFFER_SIZE_EVENT:
                flags.resize = true;
                ::ReadConsoleInputW(stdin_, &rec, 1, &n);
                break;
            case FOCUS_EVENT:
            case MENU_EVENT:
                // Spurious events — always discard.
                ::ReadConsoleInputW(stdin_, &rec, 1, &n);
                break;
            default:
                // KEY_EVENT, MOUSE_EVENT, or unknown — real input.
                flags.input = true;
                return;
            }
        }
    }
};

} // namespace maya::platform::win32
