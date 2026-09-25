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

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    #include <cerrno>
    #include <poll.h>      // poll() for non-blocking writability check
    #include <sys/ioctl.h>
    #include <sys/uio.h>   // writev()
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
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
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
// io_writev — scatter-gather write (batches multiple buffers in one syscall)
// ============================================================================

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS

struct IoVec {
    const void* base;
    std::size_t len;
};

[[nodiscard]] inline auto io_writev(
    NativeHandle h, const IoVec* vecs, int count) noexcept
    -> Result<std::size_t>
{
    // Map our IoVec to OS iovec (layout-compatible, but be explicit)
    struct iovec iov[16];
    int n = count < 16 ? count : 16;
    for (int i = 0; i < n; ++i) {
        iov[i].iov_base = const_cast<void*>(vecs[i].base);
        iov[i].iov_len  = vecs[i].len;
    }

    ssize_t written = ::writev(h, iov, n);
    if (written < 0) {
        if (errno == EINTR)
            return ok(std::size_t{0});
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return err<std::size_t>(Error::would_block());
        return err<std::size_t>(Error::from_errno("writev"));
    }
    return ok(static_cast<std::size_t>(written));
}

#endif // POSIX || MACOS

// ============================================================================
// io_read — single read, returns 0 bytes on EINTR/EAGAIN
// ============================================================================

[[nodiscard]] inline auto io_read(
    NativeHandle h, void* buf, std::size_t len) noexcept
    -> Result<std::size_t>
{
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
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
// io_poll_writable — non-blocking check for output writability
// ============================================================================
//
// Returns true if `handle` accepts at least one byte of write right now.
// On POSIX, a single poll() with POLLOUT and timeout=0. On Win32 the
// console buffer for an interactive tty is large enough that this check
// rarely matters; return true.
//
// Used by the inline renderer to detect kernel-buffer backpressure on
// slow ttys (serial console, framebuffer, ssh over high-latency link)
// and skip the paint cycle entirely — the next render coalesces the
// diff naturally because `prev_cells` wasn't updated. Modern terminals
// always return true and pay only the syscall cost (~µs).

[[nodiscard]] inline bool io_poll_writable(
    [[maybe_unused]] NativeHandle h) noexcept
{
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    struct pollfd pfd{};
    pfd.fd = h;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int r = ::poll(&pfd, 1, 0);
    if (r <= 0) {
        // 0 = timeout (kernel buffer currently full)
        // < 0 = error (EINTR / EBADF). On error, conservatively assume
        // writable so we don't block forever on a transient fault.
        return r < 0;
    }
    return (pfd.revents & POLLOUT) != 0;
#else
    return true;
#endif
}

// ============================================================================
// Terminal size query
// ============================================================================

[[nodiscard]] inline Size query_terminal_size(
    [[maybe_unused]] NativeHandle h) noexcept
{
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
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
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    return STDOUT_FILENO;
#else
    return ::GetStdHandle(STD_OUTPUT_HANDLE);
#endif
}

// ============================================================================
// stdin handle
// ============================================================================

[[nodiscard]] inline NativeHandle stdin_handle() noexcept {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    return STDIN_FILENO;
#else
    return ::GetStdHandle(STD_INPUT_HANDLE);
#endif
}

// ============================================================================
// TTY detection
// ============================================================================
//
// THE QUESTION THIS ANSWERS is "is a human looking at this", not "is this a
// Win32 console object". The two are the same on POSIX and emphatically not
// on Windows.
//
// GetConsoleMode() alone says NO for a terminal that is plainly interactive:
// under mintty (MSYS2's default), Git Bash, Cygwin, and anything driving a
// ConPTY, fds 0/1 are named PIPES carrying a VT byte stream, not console
// handles. Win32Terminal::open() already knows this -- a failed
// GetConsoleMode there means "pipe mode", not "no terminal", and that fix is
// what made agentty run under MSYS2 at all.
//
// is_tty() was never told. So every OTHER consumer of the same question kept
// getting the wrong answer, and the loudest one is colour: detect_tier()
// returns Mono for a non-tty, which silently collapses every theme to native
// and drops diff row bands to plain text. "Why is + green everywhere except
// Windows Terminal" is this function, one layer down.
//
// A pipe is only a terminal if something on the far end is drawing it, so we
// require positive evidence rather than assuming: either the host names
// itself (WT_SESSION / ConEmuANSI / a TERM an MSYS2 shell exports) or the
// handle is a pipe whose name matches the msys/cygwin PTY convention. A
// plain redirect to a file or an anonymous pipe still answers false, which
// is what keeps `agentty > log.txt` free of escape soup.
[[nodiscard]] inline bool is_tty(NativeHandle h) noexcept {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    return ::isatty(h) != 0;
#else
    DWORD mode;
    if (::GetConsoleMode(h, &mode) != 0) return true;   // real console

    // Not a console handle. It may still be a VT terminal on a pipe.
    if (::GetFileType(h) != FILE_TYPE_PIPE) return false;   // file/redirect

    // A host that identifies itself is the strongest evidence available,
    // and it does not depend on naming the pipe.
    const auto env_set = [](const char* k) noexcept {
        const char* v = std::getenv(k);
        return v != nullptr && *v != '\0';
    };
    if (env_set("WT_SESSION")) return true;          // Windows Terminal
    if (env_set("ConEmuANSI")) return true;          // ConEmu
    if (env_set("MSYSTEM"))    return true;          // MSYS2 / Git Bash

    // Otherwise ask the pipe its name. MSYS2 and Cygwin PTYs are named
    // \cygwin-<id>-pty<N>-{from,to}-master (msys- for MSYS2), which is the
    // documented way to tell a PTY from an ordinary anonymous pipe.
    struct NameInfo {
        DWORD length;
        WCHAR name[260];
    } info{};
    if (::GetFileInformationByHandleEx(h, FileNameInfo, &info, sizeof info)) {
        const std::size_t n =
            (std::min)(static_cast<std::size_t>(info.length / sizeof(WCHAR)),
                       static_cast<std::size_t>(259));
        std::wstring_view nm{info.name, n};
        const bool ptyish = nm.find(L"pty") != std::wstring_view::npos;
        if (ptyish && (nm.find(L"msys-")   != std::wstring_view::npos
                    || nm.find(L"cygwin-") != std::wstring_view::npos))
            return true;
    }
    return false;
#endif
}

// ============================================================================
// ensure_utf8 — one-shot UTF-8 console codepage setup (Win32)
// ============================================================================
// Call before any non-Terminal UTF-8 output (e.g. print()).
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
