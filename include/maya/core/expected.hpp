#pragma once
// maya::core::expected - Monadic error handling
//
// Rust has Result<T, E>. We have std::expected<T, E> (C++23) plus a rich
// Error type with source location tracking. No exceptions. No error codes.
// Just algebraic types composed monadically.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace maya {

// ============================================================================
// ErrorKind - categorized error variants
// ============================================================================

enum class ErrorKind : uint8_t {
    TerminalInit,       // Failed to initialize terminal
    Io,                 // I/O operation failed
    LayoutOverflow,     // Layout computation overflow
    InvalidStyle,       // Invalid style specification
    InvalidUtf8,        // Malformed UTF-8 input
    Unsupported,        // Feature not supported by terminal
    Signal,             // Process signal received
    WouldBlock,         // Write would block (EAGAIN/EWOULDBLOCK) — frame skipped
};

// ============================================================================
// Error - rich error type with source location
// ============================================================================

struct Error {
    ErrorKind           kind;
    std::string         message;
    std::source_location location;

    // Factory methods - named constructors for each error kind
    [[nodiscard]] static Error terminal(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::TerminalInit, std::move(msg), loc};
    }

    [[nodiscard]] static Error io(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::Io, std::move(msg), loc};
    }

    [[nodiscard]] static Error layout(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::LayoutOverflow, std::move(msg), loc};
    }

    [[nodiscard]] static Error unsupported(
        std::string msg,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::Unsupported, std::move(msg), loc};
    }

    [[nodiscard]] static Error would_block(
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::WouldBlock, "write would block", loc};
    }

    [[nodiscard]] static Error from_errno(
        std::string_view context,
        std::source_location loc = std::source_location::current()
    ) {
        return {ErrorKind::Io, std::string(context) + ": " + std::string(strerror(errno)), loc};
    }
};

// ============================================================================
// Result<T> = std::expected<T, Error>
// ============================================================================

template <typename T>
using Result = std::expected<T, Error>;

// Result<void> shorthand
using Status = Result<void>;

// ============================================================================
// ok() / err() helpers - like Rust's Ok() and Err()
// ============================================================================

template <typename T>
[[nodiscard]] constexpr Result<T> ok(T&& value) {
    return Result<T>{std::forward<T>(value)};
}

[[nodiscard]] inline Status ok() {
    return {};
}

template <typename T = void>
[[nodiscard]] constexpr std::unexpected<Error> err(Error e) {
    return std::unexpected{std::move(e)};
}

// ============================================================================
// try_() macro - like Rust's ? operator
// ============================================================================
// Usage: auto val = MAYA_TRY(some_result_returning_fn());
//
// If the result is an error, immediately returns it from the enclosing function.
// If the result is a value, unwraps it.

#define MAYA_TRY(expr)                                         \
    ({                                                         \
        auto&& _maya_result = (expr);                          \
        if (!_maya_result) [[unlikely]]                        \
            return std::unexpected{_maya_result.error()};       \
        std::move(*_maya_result);                               \
    })

// For Status (Result<void>) - just propagate error, no value to unwrap
#define MAYA_TRY_VOID(expr)                                    \
    do {                                                       \
        auto&& _maya_result = (expr);                          \
        if (!_maya_result) [[unlikely]]                        \
            return std::unexpected{_maya_result.error()};       \
    } while (0)

} // namespace maya
