#pragma once
// maya::platform::win32::Win32Terminal - Windows Console API backend
//
// Uses ENABLE_VIRTUAL_TERMINAL_PROCESSING for ANSI output and
// ENABLE_VIRTUAL_TERMINAL_INPUT for VT input sequences. This enables
// the same ANSI escape code path as POSIX — no separate Windows
// rendering pipeline needed. Requires Windows 10 1607+ / Windows Terminal.

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

#include <string>
#include <string_view>
#include <utility>

#include "../../core/expected.hpp"
#include "../../core/types.hpp"
#include "../io.hpp"

namespace maya::platform::win32 {

// ============================================================================
// Win32Terminal — Console API + VT processing backend
// ============================================================================

class Win32Terminal {
    HANDLE stdin_   = INVALID_HANDLE_VALUE;
    HANDLE stdout_  = INVALID_HANDLE_VALUE;
    DWORD  orig_in_mode_  = 0;
    DWORD  orig_out_mode_ = 0;
    UINT   orig_out_cp_   = 0;
    UINT   orig_in_cp_    = 0;
    bool   raw_ = false;

    Win32Terminal() = default;

public:
    // -- Construction ---------------------------------------------------------

    [[nodiscard]] static auto open() -> Result<Win32Terminal> {
        Win32Terminal t;
        t.stdin_  = ::GetStdHandle(STD_INPUT_HANDLE);
        t.stdout_ = ::GetStdHandle(STD_OUTPUT_HANDLE);

        if (t.stdin_ == INVALID_HANDLE_VALUE || t.stdout_ == INVALID_HANDLE_VALUE)
            return err<Win32Terminal>(Error::terminal("GetStdHandle failed"));

        if (!::GetConsoleMode(t.stdin_, &t.orig_in_mode_))
            return err<Win32Terminal>(Error::terminal("GetConsoleMode(stdin) failed"));

        if (!::GetConsoleMode(t.stdout_, &t.orig_out_mode_))
            return err<Win32Terminal>(Error::terminal("GetConsoleMode(stdout) failed"));

        // Set UTF-8 codepage for correct Unicode rendering.
        // WriteFile interprets bytes through the console output codepage —
        // without this, UTF-8 encoded characters (block elements, box drawing,
        // CJK, emoji) are garbled through the legacy system codepage.
        t.orig_out_cp_ = ::GetConsoleOutputCP();
        t.orig_in_cp_  = ::GetConsoleCP();
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);

        return ok(std::move(t));
    }

    // -- Raw mode -------------------------------------------------------------

    [[nodiscard]] auto enable_raw() -> Status {
        // Input: character-at-a-time, VT sequences, window events
        DWORD in_mode = orig_in_mode_;
        in_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
        if (!::SetConsoleMode(stdin_, in_mode))
            return err(Error::terminal("SetConsoleMode(stdin) failed"));

        // Output: VT processing for ANSI escape sequences
        DWORD out_mode = orig_out_mode_;
        out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
        if (!::SetConsoleMode(stdout_, out_mode)) {
            ::SetConsoleMode(stdin_, orig_in_mode_); // rollback
            return err(Error::terminal("SetConsoleMode(stdout) failed"));
        }

        raw_ = true;
        return ok();
    }

    [[nodiscard]] auto disable_raw() -> Status {
        if (raw_) {
            ::SetConsoleMode(stdin_,  orig_in_mode_);
            ::SetConsoleMode(stdout_, orig_out_mode_);
            raw_ = false;
        }
        // Restore original codepages
        if (orig_out_cp_ != 0) ::SetConsoleOutputCP(orig_out_cp_);
        if (orig_in_cp_  != 0) ::SetConsoleCP(orig_in_cp_);
        return ok();
    }

    // -- I/O ------------------------------------------------------------------

    [[nodiscard]] auto write_all(std::string_view data) -> Status {
        return io_write_all(stdout_, data);
    }

    [[nodiscard]] auto write_some(std::string_view data) -> Result<std::size_t> {
        return io_write(stdout_, data.data(), data.size());
    }

    [[nodiscard]] auto read_raw() -> Result<std::string> {
        // Wait briefly for input (100ms, matching POSIX VTIME=1).
        DWORD wait = ::WaitForSingleObject(stdin_, 100);
        if (wait == WAIT_TIMEOUT)
            return ok(std::string{});

        // Drain system events (resize, focus, menu) that ReadFile cannot
        // translate in VT input mode — they'd block the read indefinitely.
        INPUT_RECORD rec;
        DWORD cnt;
        while (::PeekConsoleInputW(stdin_, &rec, 1, &cnt) && cnt > 0) {
            if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT ||
                rec.EventType == FOCUS_EVENT ||
                rec.EventType == MENU_EVENT) {
                ::ReadConsoleInputW(stdin_, &rec, 1, &cnt);
            } else {
                break;  // Real input — let ReadFile handle it.
            }
        }

        // If only system events were in the queue, nothing left to read.
        if (!::PeekConsoleInputW(stdin_, &rec, 1, &cnt) || cnt == 0)
            return ok(std::string{});

        char buf[256];
        DWORD n = 0;
        if (!::ReadFile(stdin_, buf, sizeof(buf), &n, nullptr)) {
            DWORD e = ::GetLastError();
            if (e == ERROR_IO_PENDING)
                return ok(std::string{});
            return err<std::string>(Error::io("ReadFile(stdin) failed"));
        }
        return ok(std::string(buf, n));
    }

    // -- Properties -----------------------------------------------------------

    [[nodiscard]] auto size() const -> Size {
        return query_terminal_size(stdout_);
    }

    [[nodiscard]] NativeHandle input_handle()  const noexcept { return stdin_; }
    [[nodiscard]] NativeHandle output_handle() const noexcept { return stdout_; }

    // -- Move-only ------------------------------------------------------------

    Win32Terminal(Win32Terminal&& o) noexcept
        : stdin_(std::exchange(o.stdin_, INVALID_HANDLE_VALUE))
        , stdout_(std::exchange(o.stdout_, INVALID_HANDLE_VALUE))
        , orig_in_mode_(o.orig_in_mode_)
        , orig_out_mode_(o.orig_out_mode_)
        , orig_out_cp_(std::exchange(o.orig_out_cp_, 0))
        , orig_in_cp_(std::exchange(o.orig_in_cp_, 0))
        , raw_(std::exchange(o.raw_, false))
    {}

    Win32Terminal& operator=(Win32Terminal&& o) noexcept {
        if (this != &o) {
            if (raw_) {
                ::SetConsoleMode(stdin_,  orig_in_mode_);
                ::SetConsoleMode(stdout_, orig_out_mode_);
            }
            if (orig_out_cp_ != 0) ::SetConsoleOutputCP(orig_out_cp_);
            if (orig_in_cp_  != 0) ::SetConsoleCP(orig_in_cp_);

            stdin_   = std::exchange(o.stdin_, INVALID_HANDLE_VALUE);
            stdout_  = std::exchange(o.stdout_, INVALID_HANDLE_VALUE);
            orig_in_mode_  = o.orig_in_mode_;
            orig_out_mode_ = o.orig_out_mode_;
            orig_out_cp_   = std::exchange(o.orig_out_cp_, 0);
            orig_in_cp_    = std::exchange(o.orig_in_cp_, 0);
            raw_ = std::exchange(o.raw_, false);
        }
        return *this;
    }

    Win32Terminal(const Win32Terminal&) = delete;
    Win32Terminal& operator=(const Win32Terminal&) = delete;

    ~Win32Terminal() {
        if (raw_) {
            ::SetConsoleMode(stdin_,  orig_in_mode_);
            ::SetConsoleMode(stdout_, orig_out_mode_);
        }
        if (orig_out_cp_ != 0) ::SetConsoleOutputCP(orig_out_cp_);
        if (orig_in_cp_  != 0) ::SetConsoleCP(orig_in_cp_);
    }
};

} // namespace maya::platform::win32
