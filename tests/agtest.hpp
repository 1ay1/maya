// agtest — doctest compatibility shim for maya's test suite.
// Remaps assert() onto doctest CHECK (extra parens so `assert(a && b)` doesn't
// trip the expression decomposer). A test migrates by dropping <cassert>,
// including this, and wrapping its int main() body in a TEST_CASE. main() comes
// from test_main.cpp. Include INSTEAD of <doctest/doctest.h>.
#ifndef MAYA_TESTS_AGTEST_HPP
#define MAYA_TESTS_AGTEST_HPP

#include <doctest/doctest.h>

#ifdef assert
#  undef assert
#endif
#define assert(cond) DOCTEST_CHECK((cond))

// Some tests define a local 2-arg REQUIRE(cond, msg). Provide a doctest-backed
// version (wrapped parens so `REQUIRE(a && b, ...)` compiles) and let those
// files drop their local macro.
#ifdef REQUIRE
#  undef REQUIRE
#endif
#define AGTEST_REQUIRE_2(cond, msg) DOCTEST_REQUIRE_MESSAGE((cond), msg)
#define AGTEST_REQUIRE_1(cond)      DOCTEST_REQUIRE((cond))
#define AGTEST_REQUIRE_PICK(_1, _2, NAME, ...) NAME
#define REQUIRE(...) \
    AGTEST_REQUIRE_PICK(__VA_ARGS__, AGTEST_REQUIRE_2, AGTEST_REQUIRE_1)(__VA_ARGS__)

// maya's render-scaling test uses a printf-style CHECK(cond, fmt, args...).
// Route the condition into doctest and format the message so the diagnostic
// survives. Variadic: 1-arg is a bare predicate, 2+ args carry a printf-style
// message we render with a small buffer.
#ifdef CHECK
#  undef CHECK
#endif
#include <cstdarg>
#include <cstdio>
#include <string>
namespace mayatest {
inline std::string fmtmsg(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}
} // namespace mayatest
#define AGTEST_MCHECK_1(cond)        DOCTEST_CHECK((cond))
#define AGTEST_MCHECK_N(cond, ...)   DOCTEST_CHECK_MESSAGE((cond), mayatest::fmtmsg(__VA_ARGS__))
#define AGTEST_MCHECK_PICK(_1, _2, _3, _4, _5, NAME, ...) NAME
#define CHECK(...) \
    AGTEST_MCHECK_PICK(__VA_ARGS__, AGTEST_MCHECK_N, AGTEST_MCHECK_N, \
                       AGTEST_MCHECK_N, AGTEST_MCHECK_N, AGTEST_MCHECK_1)(__VA_ARGS__)

#endif // MAYA_TESTS_AGTEST_HPP
