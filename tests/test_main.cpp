// The single maya test binary. Every migrated test is a doctest TEST_CASE
// auto-registered here, linking maya once instead of ~34 separate executables.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
