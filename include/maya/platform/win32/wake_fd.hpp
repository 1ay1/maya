#pragma once
// maya::platform::win32::Win32WakeFd - Many-writer / one-reader wake.
//
// Wraps a manual-reset NT event so multiple writer threads can SetEvent
// to wake the UI thread, which waits via WaitForMultipleObjects (in
// Win32EventSource) and consumes by ResetEvent.
//
// Why manual-reset (vs auto-reset):
//   ResetEvent is called explicitly inside drain() *after* the message
//   queue has been emptied. With auto-reset, WaitForMultipleObjects
//   would clear the event before drain runs, opening a window where
//   a concurrent SetEvent races the empty-queue check in send() and
//   the wake gets lost. Manual-reset + explicit drain mirrors the
//   POSIX self-pipe protocol exactly.

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

#include <utility>

#include "../../core/expected.hpp"
#include "../io.hpp"

namespace maya::platform::win32 {

class Win32WakeFd {
    // CreateEventW returns NULL on failure (NOT INVALID_HANDLE_VALUE).
    // We normalize so checks against `invalid_handle` work uniformly.
    HANDLE event_ = nullptr;

    explicit Win32WakeFd(HANDLE h) noexcept : event_(h) {}

public:
    // ── Construction ──────────────────────────────────────────────────
    // Default-constructed instance is the "invalid wake" sentinel —
    // signal() and drain() are no-ops, native_handle() returns
    // invalid_handle, valid() returns false. Used as a degraded
    // fallback when create() fails.
    Win32WakeFd() = default;
    [[nodiscard]] static auto create() noexcept -> Result<Win32WakeFd> {
        HANDLE h = ::CreateEventW(
            /*lpEventAttributes*/  nullptr,
            /*bManualReset*/       TRUE,
            /*bInitialState*/      FALSE,
            /*lpName*/             nullptr);
        if (h == nullptr)
            return err<Win32WakeFd>(Error::io("CreateEventW failed"));
        return ok(Win32WakeFd{h});
    }

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return (event_ == nullptr) ? invalid_handle : event_;
    }

    [[nodiscard]] bool valid() const noexcept { return event_ != nullptr; }

    void signal() noexcept {
        if (event_) ::SetEvent(event_);
    }

    void drain() noexcept {
        if (event_) ::ResetEvent(event_);
    }

    // ── Move-only ─────────────────────────────────────────────────────
    Win32WakeFd(Win32WakeFd&& o) noexcept
        : event_(std::exchange(o.event_, nullptr)) {}

    Win32WakeFd& operator=(Win32WakeFd&& o) noexcept {
        if (this != &o) {
            if (event_) ::CloseHandle(event_);
            event_ = std::exchange(o.event_, nullptr);
        }
        return *this;
    }

    Win32WakeFd(const Win32WakeFd&)            = delete;
    Win32WakeFd& operator=(const Win32WakeFd&) = delete;

    ~Win32WakeFd() noexcept {
        if (event_) ::CloseHandle(event_);
    }
};

} // namespace maya::platform::win32
