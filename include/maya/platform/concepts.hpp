#pragma once
// maya::platform::concepts - Backend contracts
//
// Each concept defines what a platform backend must provide.
// This is the C++26 equivalent of Rust trait definitions —
// compile-time interface contracts that are verified with
// static_assert in select.hpp.
//
// The upper layers (Terminal<State>, App, etc.) are generic
// over these concepts — they never name a concrete backend type.

#include "../core/expected.hpp"
#include "../core/types.hpp"
#include "io.hpp"

#include <chrono>
#include <concepts>
#include <string>
#include <string_view>

namespace maya::platform {

// ============================================================================
// ReadyFlags — what became ready after a wait
// ============================================================================

struct ReadyFlags {
    bool input     : 1 = false;   // stdin has data
    bool resize    : 1 = false;   // terminal was resized
    bool writeable : 1 = false;   // stdout can accept more data
};

// ============================================================================
// TerminalBackend — raw terminal I/O
// ============================================================================
// Owns the terminal state. Provides raw mode toggling, byte-level I/O,
// and size queries. POSIX: termios. Win32: Console API.

template <typename T>
concept TerminalBackend = requires(T& t, const T& ct, std::string_view sv) {
    // Construction
    { T::open() } -> std::same_as<Result<T>>;

    // Raw mode toggle
    { t.enable_raw() } -> std::same_as<Status>;
    { t.disable_raw() } -> std::same_as<Status>;

    // I/O
    { t.write_all(sv) } -> std::same_as<Status>;
    { t.write_some(sv) } -> std::same_as<Result<std::size_t>>;
    { t.read_raw() } -> std::same_as<Result<std::string>>;

    // Properties
    { ct.size() } -> std::same_as<Size>;
    { ct.input_handle() } -> std::same_as<NativeHandle>;
    { ct.output_handle() } -> std::same_as<NativeHandle>;
} && std::movable<T> && (!std::copyable<T>);

// ============================================================================
// EventMultiplexer — waits for I/O + signal readiness
// ============================================================================
// POSIX: poll(). Win32: WaitForMultipleObjects().

template <typename T>
concept EventMultiplexer = requires(T& t, std::chrono::milliseconds ms, bool want_write) {
    { t.wait(ms, want_write) } -> std::same_as<Result<ReadyFlags>>;
};

// ============================================================================
// ResizeSource — detects terminal resize events
// ============================================================================
// POSIX: sigaction(SIGWINCH) + self-pipe.
// Win32: console size polling.

template <typename T>
concept ResizeSource = requires(T& t) {
    { T::install() } -> std::same_as<Result<T>>;
    { t.pending() } -> std::same_as<bool>;
    { t.drain() };
    { t.native_handle() } -> std::same_as<NativeHandle>;
} && std::movable<T> && (!std::copyable<T>);

} // namespace maya::platform
