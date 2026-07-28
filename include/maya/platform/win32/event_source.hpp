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

#include <algorithm>
#include <chrono>
#include <thread>

#include "../../core/expected.hpp"
#include "../concepts.hpp"
#include "../io.hpp"

namespace maya::platform::win32 {

// ============================================================================
// Win32EventSource — console event-aware multiplexer
// ============================================================================

class Win32EventSource {
    HANDLE stdin_;
    HANDLE wake_ = INVALID_HANDLE_VALUE;
    // stdin is a pipe (mintty / MSYS2 PTY), not a console. The console input
    // APIs used to drain untranslatable records don't apply; a signaled wait
    // means raw VT bytes are ready to ReadFile. Detected once from the handle
    // type so the EventMultiplexer ctor signature stays unchanged.
    bool   pipe_ = false;

public:
    Win32EventSource(NativeHandle term_in, [[maybe_unused]] NativeHandle sig_handle) noexcept
        : stdin_(term_in)
        , pipe_(::GetFileType(term_in) == FILE_TYPE_PIPE)
    {}

    void set_wake_handle(HANDLE h) noexcept {
        // CreateEventW returns NULL on failure; INVALID_HANDLE_VALUE is a
        // separate sentinel. Accept either as "no wake" so a failed
        // BackgroundQueue construction (silently passing NULL) doesn't
        // get added to WaitForMultipleObjects, which would fail it
        // outright with ERROR_INVALID_HANDLE.
        wake_ = (h == nullptr) ? INVALID_HANDLE_VALUE : h;
    }

    [[nodiscard]] auto wait(
        std::chrono::milliseconds timeout,
        [[maybe_unused]] bool want_write = false) -> Result<ReadyFlags>
    {
        ReadyFlags flags{};
        flags.writeable = true;

        // Cap to INFINITE if the chrono value would overflow DWORD —
        // happens if a caller passes std::chrono::milliseconds::max() or
        // similar "wait forever" sentinel. Direct cast would wrap to a
        // small value and busy-poll.
        const auto count = timeout.count();
        DWORD ms = (count < 0)
                        ? 0u
                 : (count > static_cast<long long>(INFINITE - 1))
                        ? INFINITE
                        : static_cast<DWORD>(count);

        // Pipe path (mintty / MSYS2 PTY): the stdin HANDLE is a byte-mode
        // named pipe. A named-pipe handle is NOT a data-ready synchronization
        // object -- WaitForSingleObject on it does not block until bytes
        // arrive (it reflects I/O-completion / handle state, so it would
        // busy-spin at ms=0). This is exactly why Cygwin's own select()
        // implementation polls with PeekNamedPipe instead of waiting on the
        // handle. We mirror that: PeekNamedPipe for input readiness, and
        // (when present) wait on the wake event -- which IS a real waitable
        // object -- so background-task wakeups stay latency-free.
        if (pipe_) {
            return wait_pipe(ms, flags);
        }

        if (wake_ != INVALID_HANDLE_VALUE) {
            HANDLE handles[2] = { stdin_, wake_ };
            DWORD result = ::WaitForMultipleObjects(2, handles, FALSE, ms);
            if (result == WAIT_FAILED) {
                // Hard kernel error (handle invalidated, etc.). Surface it
                // so the runtime can shut down cleanly instead of spinning
                // on a broken wait.
                return err<ReadyFlags>(Error::io("WaitForMultipleObjects failed"));
            }
            // WAIT_TIMEOUT and WAIT_OBJECT_0+i both leave us probing each
            // handle individually below; WaitForMultipleObjects only
            // reports the lowest signaled index.
            if (::WaitForSingleObject(stdin_, 0) == WAIT_OBJECT_0)
                mark_input_ready(flags);
            if (::WaitForSingleObject(wake_, 0) == WAIT_OBJECT_0)
                flags.wake = true;
        } else {
            DWORD result = ::WaitForSingleObject(stdin_, ms);
            if (result == WAIT_FAILED)
                return err<ReadyFlags>(Error::io("WaitForSingleObject failed"));
            if (result == WAIT_OBJECT_0) {
                mark_input_ready(flags);
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
    // Poll a byte-mode named pipe (MSYS2 PTY) for input readiness up to `ms`.
    // Cygwin's runtime on the far end of this pipe drives select() the same
    // way: PeekNamedPipe to see whether unread bytes are buffered. We slice
    // the timeout into short quanta so a concurrent wake (SetEvent on the
    // manual-reset wake handle -- a genuinely waitable object) is observed
    // with sub-quantum latency, and so an idle wait costs almost no CPU.
    [[nodiscard]] auto wait_pipe(DWORD ms, ReadyFlags flags) -> Result<ReadyFlags>
    {
        using clock = std::chrono::steady_clock;
        const bool infinite = (ms == INFINITE);
        const auto deadline = clock::now() + std::chrono::milliseconds(ms);

        // Poll quantum: small enough to feel instant, large enough to keep
        // the idle CPU cost negligible. Wake latency is bounded by this.
        constexpr DWORD kQuantumMs = 5;

        for (;;) {
            // 1) Data already buffered in the pipe? -> input ready, return now.
            DWORD avail = 0;
            if (::PeekNamedPipe(stdin_, nullptr, 0, nullptr, &avail, nullptr)) {
                if (avail > 0) {
                    flags.input = true;
                    return ok(ReadyFlags{flags});
                }
            } else {
                // Peek failed: the writer closed its end (broken pipe / EOF).
                // Flag input (so read_raw runs and observes EOF) AND hangup
                // (so the runtime tears down) rather than spinning forever.
                flags.input = true;
                flags.hangup = true;
                return ok(ReadyFlags{flags});
            }

            // 2) Wake event pending? (background task signalled the UI thread.)
            if (wake_ != INVALID_HANDLE_VALUE &&
                ::WaitForSingleObject(wake_, 0) == WAIT_OBJECT_0) {
                flags.wake = true;
                return ok(ReadyFlags{flags});
            }

            // 3) Timed out?
            if (!infinite && clock::now() >= deadline)
                return ok(ReadyFlags{flags});

            // 4) Sleep one quantum, capped at the remaining time. If a wake
            //    handle exists, block on IT for the quantum so a SetEvent
            //    returns us immediately instead of after the full sleep.
            DWORD slice = kQuantumMs;
            if (!infinite) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - clock::now()).count();
                if (left <= 0) return ok(ReadyFlags{flags});
                slice = static_cast<DWORD>(std::min<long long>(kQuantumMs, left));
            }
            if (wake_ != INVALID_HANDLE_VALUE) {
                if (::WaitForSingleObject(wake_, slice) == WAIT_OBJECT_0) {
                    flags.wake = true;
                    return ok(ReadyFlags{flags});
                }
            } else {
                ::Sleep(slice);
            }
        }
    }

    // A signaled stdin means input is ready. For a console we first drain the
    // untranslatable records (resize/focus/key-up) that would otherwise stall
    // the byte read; for a pipe there are no such records -- a signal means
    // raw VT bytes are waiting, so just flag input.
    void mark_input_ready(ReadyFlags& flags) noexcept {
        if (pipe_) { flags.input = true; return; }
        drain_system_events(flags);
    }

    // Peek at the front of the console input queue. Consume events that
    // ReadFile cannot translate in VT mode (resize, focus, menu) so they
    // don't block the byte-oriented read path. Stop at the first real
    // input event (KEY_EVENT, MOUSE_EVENT) — those stay for ReadFile.
    void drain_system_events(ReadyFlags& flags) noexcept {
        INPUT_RECORD rec;
        DWORD n;
        while (::PeekConsoleInputW(stdin_, &rec, 1, &n) && n > 0) {
            switch (rec.EventType) {
            case KEY_EVENT:
                if (rec.Event.KeyEvent.bKeyDown) {
                    // A real key-down: ReadFile can translate it to bytes.
                    // Leave it in the queue for the read path to consume.
                    flags.input = true;
                    return;
                }
                // Key-UP: produces no VT byte. Leaving it queued falsely
                // flags input readiness AND stalls the subsequent blocking
                // ReadFile until a real keypress. WezTerm-on-Windows leaves
                // the submit-Enter's key-up in the queue, which froze every
                // animation (spinner / streaming reveal). Drain it.
                ::ReadConsoleInputW(stdin_, &rec, 1, &n);
                break;
            case WINDOW_BUFFER_SIZE_EVENT:
                flags.resize = true;
                ::ReadConsoleInputW(stdin_, &rec, 1, &n);
                break;
            default:
                // FOCUS / MENU / MOUSE / any other record is also opaque to
                // ReadFile in VT-input mode — drain so it can neither flag
                // input nor stall the read path.
                ::ReadConsoleInputW(stdin_, &rec, 1, &n);
                break;
            }
        }
    }
};

} // namespace maya::platform::win32
