#pragma once
// maya::platform::io - Platform-abstracted I/O primitives
//
// Provides NativeHandle (int on POSIX, HANDLE on Win32) and thin
// wrappers around the OS write/read syscalls with proper error
// handling. Used by Writer and the terminal backends.

#include "detect.hpp"
#include "../core/expected.hpp"
#include "../core/types.hpp"

#include <cstddef>
#include <string_view>

#if MAYA_PLATFORM_POSIX
    #include <cerrno>
    #include <sys/ioctl.h>
    #include <unistd.h>
#elif MAYA_PLATFORM_WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace maya::platform {

// ============================================================================
// NativeHandle — the OS-level I/O object
// ============================================================================

#if MAYA_PLATFORM_WIN32
using NativeHandle = void*;   // HANDLE
#else
using NativeHandle = int;     // file descriptor
#endif

// ============================================================================
// Sentinel values
// ============================================================================

#if MAYA_PLATFORM_WIN32
inline const NativeHandle invalid_handle = INVALID_HANDLE_VALUE;
#else
inline constexpr NativeHandle invalid_handle = -1;
#endif

// ============================================================================
// io_write — single write, may return short count
// ============================================================================

[[nodiscard]] inline auto io_write(
    NativeHandle h, const void* data, std::size_t len) noexcept
    -> Result<std::size_t>
{
#if MAYA_PLATFORM_POSIX
    ssize_t n = ::write(h, data, len);
    if (n < 0) {
        if (errno == EINTR)
            return ok(std::size_t{0});
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return err<std::size_t>(Error::would_block());
        return err<std::size_t>(Error::from_errno("write"));
    }
    return ok(static_cast<std::size_t>(n));
#else
    DWORD written = 0;
    if (!::WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr)) {
        DWORD e = ::GetLastError();
        if (e == ERROR_IO_PENDING)
            return err<std::size_t>(Error::would_block());
        return err<std::size_t>(Error::io("WriteFile failed"));
    }
    return ok(static_cast<std::size_t>(written));
#endif
}

// ============================================================================
// io_write_all — retry until all bytes written
// ============================================================================

[[nodiscard]] inline auto io_write_all(
    NativeHandle h, std::string_view data) noexcept -> Status
{
    const char* ptr = data.data();
    std::size_t remaining = data.size();

    while (remaining > 0) {
        auto result = io_write(h, ptr, remaining);
        if (!result) return std::unexpected{result.error()};

        std::size_t n = *result;
        if (n == 0) continue;   // EINTR retry
        ptr       += n;
        remaining -= n;
    }
    return ok();
}

// ============================================================================
// io_read — single read, returns 0 bytes on EINTR/EAGAIN
// ============================================================================

[[nodiscard]] inline auto io_read(
    NativeHandle h, void* buf, std::size_t len) noexcept
    -> Result<std::size_t>
{
#if MAYA_PLATFORM_POSIX
    ssize_t n = ::read(h, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return ok(std::size_t{0});
        return err<std::size_t>(Error::from_errno("read"));
    }
    return ok(static_cast<std::size_t>(n));
#else
    DWORD bytes_read = 0;
    if (!::ReadFile(h, buf, static_cast<DWORD>(len), &bytes_read, nullptr)) {
        DWORD e = ::GetLastError();
        if (e == ERROR_IO_PENDING)
            return ok(std::size_t{0});
        return err<std::size_t>(Error::io("ReadFile failed"));
    }
    return ok(static_cast<std::size_t>(bytes_read));
#endif
}

// ============================================================================
// Terminal size query
// ============================================================================

[[nodiscard]] inline Size query_terminal_size(
    [[maybe_unused]] NativeHandle h) noexcept
{
#if MAYA_PLATFORM_POSIX
    struct winsize ws{};
    if (::ioctl(h, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return {Columns{ws.ws_col}, Rows{ws.ws_row}};
    return {Columns{80}, Rows{24}};
#else
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (::GetConsoleScreenBufferInfo(h, &info))
        return {
            Columns{info.srWindow.Right - info.srWindow.Left + 1},
            Rows{info.srWindow.Bottom - info.srWindow.Top + 1}
        };
    return {Columns{80}, Rows{24}};
#endif
}

// ============================================================================
// stdout handle
// ============================================================================

[[nodiscard]] inline NativeHandle stdout_handle() noexcept {
#if MAYA_PLATFORM_POSIX
    return STDOUT_FILENO;
#else
    return ::GetStdHandle(STD_OUTPUT_HANDLE);
#endif
}

// ============================================================================
// stdin handle
// ============================================================================

[[nodiscard]] inline NativeHandle stdin_handle() noexcept {
#if MAYA_PLATFORM_POSIX
    return STDIN_FILENO;
#else
    return ::GetStdHandle(STD_INPUT_HANDLE);
#endif
}

// ============================================================================
// TTY detection
// ============================================================================

[[nodiscard]] inline bool is_tty(NativeHandle h) noexcept {
#if MAYA_PLATFORM_POSIX
    return ::isatty(h) != 0;
#else
    DWORD mode;
    return ::GetConsoleMode(h, &mode) != 0;
#endif
}

// ============================================================================
// ensure_utf8 — one-shot UTF-8 console codepage setup (Win32)
// ============================================================================
// Call before any non-Terminal UTF-8 output (e.g. print(), live()).
// No-op on POSIX. Safe to call multiple times.

inline void ensure_utf8() noexcept {
#if MAYA_PLATFORM_WIN32
    static bool done = false;
    if (!done) {
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);
        done = true;
    }
#endif
}

} // namespace maya::platform
