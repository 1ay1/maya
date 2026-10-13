// tests/check.hpp — A hard assertion that survives -DNDEBUG.
//
// CMake builds these tests in Release by default (-O3 -DNDEBUG), which
// silently strips every assert(). Tests then exit 0 regardless of
// whether their invariants held. Use CHECK(cond, msg) instead.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string_view>

#define MAYA_TEST_CHECK(cond, msg) do {                                       \
    if (!(cond)) {                                                            \
        std::println("\n  HARD FAIL at {}:{}\n    cond:  {}\n    msg:   {}",  \
                     __FILE__, __LINE__,                                      \
                     std::string_view{#cond},                                 \
                     std::string_view{(msg)});                                \
        std::abort();                                                         \
    }                                                                         \
} while (0)
