#pragma once
// maya::platform::macos::MacWakeFd - macOS uses the POSIX pipe variant.
//
// macOS doesn't have eventfd, so the WakeFd is just a self-pipe — same
// implementation as non-Linux POSIX. This file is a thin re-export so
// the platform/select.hpp aliasing is uniform across platforms.

#if !MAYA_PLATFORM_MACOS
#error "This header is for macOS only"
#endif

#include "../posix/wake_fd.hpp"

namespace maya::platform::macos {
using MacWakeFd = posix::PosixWakeFd;
} // namespace maya::platform::macos
