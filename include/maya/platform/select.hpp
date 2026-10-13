#pragma once
// maya::platform::select - Zero-cost backend selection
//
// The entire platform abstraction collapses to a type alias.
// No vtable. No indirection. The compiler sees the concrete type
// and generates identical code to hand-written platform-specific code.
//
// Concept verification happens HERE — if a backend doesn't satisfy
// its concept, compilation fails with a clear error at this point,
// not deep in some template instantiation.

#include "detect.hpp"
#include "concepts.hpp"

#if MAYA_PLATFORM_MACOS
    #include "macos/terminal.hpp"
#elif MAYA_PLATFORM_POSIX
    #include "posix/terminal.hpp"
#elif MAYA_PLATFORM_WIN32
    #include "win32/terminal.hpp"
#endif

namespace maya::platform {

// ============================================================================
// Backend type aliases — the ONLY types the rest of maya uses
// ============================================================================

#if MAYA_PLATFORM_MACOS
    using NativeTerminal     = macos::MacTerminal;
#elif MAYA_PLATFORM_WIN32
    using NativeTerminal     = win32::Win32Terminal;
#else
    using NativeTerminal     = posix::PosixTerminal;
#endif

// ============================================================================
// Compile-time contract verification
// ============================================================================
// If a backend fails to satisfy its concept, compilation stops HERE
// with a readable error — not 50 levels deep in a template.

static_assert(TerminalBackend<NativeTerminal>,
    "Native terminal backend does not satisfy TerminalBackend concept. "
    "Check that all required methods (open, enable_raw, disable_raw, "
    "write_all, write_some, read_raw, size, input_handle, output_handle) "
    "are implemented with the correct signatures.");

} // namespace maya::platform
