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

#include <cstdlib>
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
    // pipe_mode_: the inherited std handles are NOT Win32 console handles.
    // This is the mintty (MSYS2's default terminal) case -- fds 0/1 are
    // MSYS2 PTYs backed by named pipes. mintty is itself a full VT terminal
    // emulator: it already delivers raw VT INPUT sequences on the pipe and
    // renders VT OUTPUT written to the pipe, so there is NO Console API mode
    // to set (and CONIN$/CONOUT$ would address a HIDDEN console mintty never
    // shows). In pipe mode we therefore skip every Console API call and just
    // read/write raw bytes on the inherited handles -- the native, shim-free
    // path that makes agentty run under mintty exactly as it does under a
    // real console. Detected once in open(); gates raw-mode + codepage calls.
    bool   pipe_mode_ = false;
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

        // Probe whether the inherited handles are real consoles. GetConsoleMode
        // succeeds ONLY on console handles; under mintty (pipe handles) it
        // fails with ERROR_INVALID_HANDLE. The classic port treated that
        // failure as fatal ("GetConsoleMode(stdin) failed") -- the exact
        // "agentty won't run under MSYS2" symptom. Instead, fall into pipe
        // mode: mintty is a VT terminal on the other end of the pipe, so we
        // need no Console API at all.
        const bool in_is_console  = ::GetConsoleMode(t.stdin_,  &t.orig_in_mode_)  != 0;
        const bool out_is_console = ::GetConsoleMode(t.stdout_, &t.orig_out_mode_) != 0;

        if (!in_is_console || !out_is_console) {
            // Mixed console/pipe (e.g. input piped, output to console) is rare
            // for an interactive TUI and can't be raw-driven coherently; treat
            // the whole terminal as a VT pipe. Bytes flow raw on both fds.
            t.pipe_mode_ = true;
            t.orig_in_mode_  = 0;
            t.orig_out_mode_ = 0;
            return ok(std::move(t));
        }

        // Real console path (cmd / PowerShell / Windows Terminal running the
        // native exe). Set UTF-8 codepage for correct Unicode rendering:
        // WriteFile interprets bytes through the console output codepage --
        // without this, UTF-8 block/box/CJK/emoji glyphs garble through the
        // legacy system codepage.
        t.orig_out_cp_ = ::GetConsoleOutputCP();
        t.orig_in_cp_  = ::GetConsoleCP();
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);

        return ok(std::move(t));
    }

    // -- Raw mode -------------------------------------------------------------

    [[nodiscard]] auto enable_raw() -> Status {
        // Pipe mode (mintty): the terminal on the far end of the pipe already
        // delivers raw VT input and interprets VT output. There is no Console
        // API mode to change -- reading/writing raw bytes on the inherited
        // handles IS raw mode. Nothing to do.
        if (pipe_mode_) { raw_ = true; return ok(); }

        // Input: character-at-a-time, VT sequences, window events.
        //
        // We also turn OFF three default-on conhost flags that ruin TUI feel:
        //   - ENABLE_QUICK_EDIT_MODE: a stray click-drag in the terminal
        //     freezes all WriteFile output until the user presses a key.
        //     This is the #1 source of "Windows TUI feels laggy" — output
        //     stalls look identical to a slow renderer to the user.
        //   - ENABLE_MOUSE_INPUT: even without VT mouse tracking, the
        //     console pushes MOUSE_EVENT records into stdin on every mouse
        //     move, waking the event loop dozens of times per second and
        //     forcing wasted PeekConsoleInput/ReadFile churn.
        //   - ENABLE_INSERT_MODE: alters how typed/pasted text is processed
        //     under the hood; we want raw input.
        //
        // Changing QUICK_EDIT or INSERT requires ENABLE_EXTENDED_FLAGS in the
        // same SetConsoleMode call — without it, those bits are silently
        // ignored by conhost. orig_in_mode_ captured before we set the bit
        // restores cleanly on disable_raw().
        DWORD in_mode = orig_in_mode_;
        in_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
                   | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE);
        in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT
                 | ENABLE_EXTENDED_FLAGS;
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
        if (pipe_mode_) { raw_ = false; return ok(); }
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
        // Pipe mode (mintty): stdin is a named pipe, not a console. The
        // console input APIs (PeekConsoleInputW / ReadConsoleInputW /
        // GetConsoleScreenBufferInfo) all fail on it, so read the raw VT
        // byte stream mintty already delivers with a plain ReadFile. The
        // event source has confirmed readiness before we get here.
        if (pipe_mode_) {
            char buf[256];
            DWORD n = 0;
            if (!::ReadFile(stdin_, buf, sizeof(buf), &n, nullptr)) {
                const DWORD e = ::GetLastError();
                if (e == ERROR_IO_PENDING || e == ERROR_NO_DATA)
                    return ok(std::string{});
                // Pipe closed (terminal exit) -> EOF, surface as empty.
                if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF)
                    return ok(std::string{});
                return err<std::string>(Error::io("ReadFile(pipe stdin) failed"));
            }
            return ok(std::string(buf, n));
        }

        // Non-blocking readiness check. The caller (Runtime::poll →
        // Win32EventSource::wait) has already blocked on
        // WaitForMultipleObjects and confirmed stdin is signaled with a
        // real input event in the queue, so a 100 ms wait here is just
        // dead latency on the false-positive path (e.g. drain consumed
        // the trailing system events between poll and read_raw). Use a
        // zero-timeout poll to keep the early-out for that case without
        // ever blocking the event loop.
        DWORD wait = ::WaitForSingleObject(stdin_, 0);
        if (wait == WAIT_TIMEOUT)
            return ok(std::string{});

        // Drain system events (resize, focus, menu) that ReadFile cannot
        // translate in VT input mode — they'd block the read indefinitely.
        INPUT_RECORD rec;
        DWORD cnt;
        while (::PeekConsoleInputW(stdin_, &rec, 1, &cnt) && cnt > 0) {
            // ONLY a key-DOWN produces a byte for ReadFile in VT-input mode.
            // Anything else at the front of the queue — key-UP, mouse, focus,
            // menu, resize — yields no byte, so the *blocking* ReadFile below
            // would consume it and then wait for the next translatable key,
            // i.e. until the user presses something. That is the WezTerm-on-
            // Windows freeze: after the Enter that submits a turn, conhost
            // leaves Enter's key-UP record in the queue (Windows Terminal's
            // ConPTY suppresses it); it flags "input ready," ReadFile eats it
            // and blocks for seconds, freezing the spinner/stream until a
            // keypress. Drain every non-key-down record so ReadFile is only
            // entered when it will return immediately.
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
                break;
            ::ReadConsoleInputW(stdin_, &rec, 1, &cnt);
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
        if (pipe_mode_) {
            // No console screen buffer to query under mintty. Prefer the
            // shell-provided COLUMNS/LINES (MSYS2 exports them), else a
            // sane 80x24 default; a subsequent resize/DSR round-trip driven
            // by the app corrects it. Never fail -- a zero size would make
            // the renderer divide by zero / lay out nothing.
            auto env_dim = [](const char* k, int fallback) -> int {
                const char* v = std::getenv(k);
                if (!v || !*v) return fallback;
                const int n = std::atoi(v);
                return n > 0 ? n : fallback;
            };
            return Size{ Columns{env_dim("COLUMNS", 80)}, Rows{env_dim("LINES", 24)} };
        }
        return query_terminal_size(stdout_);
    }

    [[nodiscard]] NativeHandle input_handle()  const noexcept { return stdin_; }
    [[nodiscard]] NativeHandle output_handle() const noexcept { return stdout_; }

    // -- Move-only ------------------------------------------------------------

    Win32Terminal(Win32Terminal&& o) noexcept
        : stdin_(std::exchange(o.stdin_, INVALID_HANDLE_VALUE))
        , stdout_(std::exchange(o.stdout_, INVALID_HANDLE_VALUE))
        , pipe_mode_(std::exchange(o.pipe_mode_, false))
        , orig_in_mode_(o.orig_in_mode_)
        , orig_out_mode_(o.orig_out_mode_)
        , orig_out_cp_(std::exchange(o.orig_out_cp_, 0))
        , orig_in_cp_(std::exchange(o.orig_in_cp_, 0))
        , raw_(std::exchange(o.raw_, false))
    {}

    Win32Terminal& operator=(Win32Terminal&& o) noexcept {
        if (this != &o) {
            if (raw_ && !pipe_mode_) {
                ::SetConsoleMode(stdin_,  orig_in_mode_);
                ::SetConsoleMode(stdout_, orig_out_mode_);
            }
            if (orig_out_cp_ != 0) ::SetConsoleOutputCP(orig_out_cp_);
            if (orig_in_cp_  != 0) ::SetConsoleCP(orig_in_cp_);

            stdin_   = std::exchange(o.stdin_, INVALID_HANDLE_VALUE);
            stdout_  = std::exchange(o.stdout_, INVALID_HANDLE_VALUE);
            pipe_mode_     = std::exchange(o.pipe_mode_, false);
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
        // In pipe mode we never changed any console state, so there is
        // nothing to restore and the inherited handles are not ours to close.
        if (pipe_mode_) return;
        if (raw_) {
            ::SetConsoleMode(stdin_,  orig_in_mode_);
            ::SetConsoleMode(stdout_, orig_out_mode_);
        }
        if (orig_out_cp_ != 0) ::SetConsoleOutputCP(orig_out_cp_);
        if (orig_in_cp_  != 0) ::SetConsoleCP(orig_in_cp_);
    }
};

} // namespace maya::platform::win32
