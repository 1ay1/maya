#pragma once
// src/app/device_internal.hpp — shared by the Device's .cpp files only:
// the opt-in diagnostics (MAYA_IO_LOG, MAYA_INPUT_LOG, MAYA_FRAME_PROF).
#include "maya/app/app.hpp"
#include "maya/app/wire_coalesce.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // memmove: grid snapshot shift on scrollback commit
#include <format>
#include <utility>  // std::exchange: drain grid_pending_commit_
#include <thread>   // DSR cursor-position query: brief sleep between polls

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    #include <unistd.h>   // isatty, getpid, fileno
#endif

#include "maya/core/overload.hpp"
#include "maya/core/scope_exit.hpp"
#include "maya/platform/select.hpp"
#include "maya/platform/thread.hpp"
#include "maya/terminal/ansi.hpp"
#include "maya/terminal/tmux.hpp"
#include "maya/render/grid_emit.hpp"

namespace maya::detail {

inline namespace device_diag {
// Opt-in event-loop diagnostics. Set MAYA_IO_LOG=<path> to trace the
// poll/wait/read/render boundary to a file; no-op (one getenv) otherwise.
// Diagnostic scaffolding for the WezTerm-on-Windows "frozen animation"
// investigation — safe to leave compiled in.
inline void io_log(const char* fmt, ...) {
    static const char* path = std::getenv("MAYA_IO_LOG");
    if (!path) return;
    // APPEND, not truncate. app.hpp's loop_dbg() writes to the SAME path
    // through its own FILE*; a "w" here truncates the file out from under
    // it, so half the trace silently vanishes and an investigation reads
    // "that code never ran" when it ran fine. Both handles append.
    static std::FILE* f = std::fopen(path, "a");
    if (!f) return;
    const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(f, "[%9lld] ", static_cast<long long>(t));
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}
} // namespace device_diag

inline namespace device_diag {
// MAYA_INPUT_LOG=<path>: every byte the terminal sends, and what the parser
// made of it. One line per read(), then one per event, so a key that
// "didn't work" can be followed from the wire to the program: did it arrive,
// was it parsed as that key, or was it taken for a terminal reply. Costs one
// getenv when off.
//
//   [   1234] in  7B  "\x1b[0nq"
//   [   1234]   ack (DSR reply)
//   [   1234]   key char 'q' (U+0071) mods=0
inline std::FILE* input_log() {
    static std::FILE* f = [] () -> std::FILE* {
        const char* p = std::getenv("MAYA_INPUT_LOG");
        return (p && *p) ? std::fopen(p, "a") : nullptr;
    }();
    return f;
}
inline std::string escaped(std::string_view data) {
    std::string o;
    for (unsigned char c : data) {
        char b[8];
        if (c == 0x1b)                  o += "\\x1b";
        else if (c < 0x20 || c >= 0x7f) { std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
        else if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else                            o += char(c);
    }
    return o;
}
inline void log_input_bytes(std::FILE* f, std::string_view data) {
    const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(f, "[%9lld] in %3zuB  \"%s\"\n", static_cast<long long>(t), data.size(),
                 escaped(data).c_str());
}
inline void log_input_event(std::FILE* f, const Event& ev) {
    std::visit([&](const auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, KeyEvent>) {
            std::visit([&](const auto& k) {
                using K = std::decay_t<decltype(k)>;
                if constexpr (std::is_same_v<K, CharKey>)
                    std::fprintf(f, "            key char U+%04X '%c'%s%s%s%s  (from \"%s\")\n",
                                 static_cast<unsigned>(k.codepoint),
                                 k.codepoint >= 0x20 && k.codepoint < 0x7f ? static_cast<char>(k.codepoint) : '?',
                                 e.mods.ctrl ? " ctrl" : "", e.mods.alt ? " alt" : "",
                                 e.mods.shift ? " shift" : "", e.mods.super_ ? " super" : "",
                                 escaped(e.raw_sequence).c_str());
                else
                    std::fprintf(f, "            key special %d%s%s%s  (from \"%s\")\n",
                                 static_cast<int>(k), e.mods.ctrl ? " ctrl" : "",
                                 e.mods.alt ? " alt" : "", e.mods.shift ? " shift" : "",
                                 escaped(e.raw_sequence).c_str());
            }, e.key);
        } else if constexpr (std::is_same_v<E, MouseEvent>)  std::fputs("            mouse\n", f);
        else if constexpr (std::is_same_v<E, PasteEvent>)    std::fprintf(f, "            paste %zuB\n", e.content.size());
        else if constexpr (std::is_same_v<E, FocusEvent>)    std::fprintf(f, "            focus %d\n", e.focused ? 1 : 0);
        else if constexpr (std::is_same_v<E, ResizeEvent>)   std::fputs("            resize\n", f);
    }, ev);
}
} // namespace device_diag

// MAYA_FRAME_PROF: per-frame timing, shared by the inline and fullscreen
// paths. Resolved once; null when profiling is off.
inline FILE* frame_prof_out() {
    // MAYA_FRAME_PROF=1 enables per-frame timing output. Writing to a
    // tty-attached stderr while inline mode owns stdout would interleave
    // prof lines with cell bytes — visible garbage in the inline area
    // and a permanently stale prev_cells (the renderer doesn't track
    // foreign writes). Resolution:
    //
    //   - MAYA_FRAME_PROF=/path/to/log     → open that path (append).
    //   - MAYA_FRAME_PROF=1 with stderr already redirected (not a tty)
    //     → keep stderr; the user already pointed it somewhere safe.
    //   - MAYA_FRAME_PROF=1 with stderr on a tty
    //     → open /tmp/maya-frame-prof-<pid>.log instead. Deterministic
    //       path; documented here so devs know where to tail from.
    static FILE* const out = []() -> FILE* {
        const char* env = std::getenv("MAYA_FRAME_PROF");
        if (!env || !*env) return nullptr;
        if (env[0] == '/' || env[0] == '.' || env[0] == '~') {
            FILE* fp = std::fopen(env, "a");
            return fp ? fp : nullptr;
        }
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
        if (!::isatty(::fileno(stderr))) return stderr;
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/maya-frame-prof-%d.log",
                      static_cast<int>(::getpid()));
        FILE* fp = std::fopen(path, "a");
        return fp ? fp : nullptr;   // suppress if we can't open; corrupting the tty is worse
#else
        return stderr;
#endif
    }();
    return out;
}

// Milliseconds since t0, for the profile line.
inline double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count() / 1000.0;
}

} // namespace maya::detail
