#pragma once
// maya::platform::thread - Cross-platform thread hints
//
// Small helpers for telling the OS scheduler about the calling thread's
// role. Unlike the `*EventSource` / `*ResizeSignal` backends which need
// per-platform classes, these are pure inline functions — the underlying
// API is one syscall on each OS, so a header-only abstraction is the
// natural shape.

#include "detect.hpp"

#if MAYA_PLATFORM_MACOS
#include <pthread.h>
#endif

namespace maya::platform {

// ============================================================================
// set_ui_thread_priority — schedule hint for the main UI thread
// ============================================================================
//
// Mark the calling thread as user-interactive — the OS scheduler will
// prioritise it for low-jitter input/render latency.
//
//   - macOS: QOS_CLASS_USER_INTERACTIVE via pthread_set_qos_class_self_np.
//            Equivalent to AppKit's main thread; bumps scheduling
//            priority and disables background QoS demotion.
//   - Linux: no-op. SCHED_FIFO/RR exist but require CAP_SYS_NICE; nice(2)
//            is the only unprivileged knob and TUIs typically don't
//            benefit from it.
//   - Win32: no-op. SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL) is
//            a candidate but is too aggressive for a process that's
//            mostly idle waiting on input; the default scheduler treats
//            interactive processes well already.
//
// Idempotent and noexcept — safe to call repeatedly. Errors are
// ignored (best-effort hint).

inline void set_ui_thread_priority() noexcept {
#if MAYA_PLATFORM_MACOS
    (void)::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

} // namespace maya::platform
