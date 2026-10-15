# run_case.cmake — one compile-fail case from tests/compile_fail/seam.cpp.
#
#   cmake -DCXX=<compiler> -DSTD=<flag> -DINC=<dir;dir> -DSRC=<file>
#         -DCASE=<n> [-DMATCH=<regex>] [-DEXPECT_OK=1] [-DSYSROOT=<sdk>]
#         -P run_case.cmake
#
# EXPECT_OK: the case is a control and must compile. Otherwise the compile
# must FAIL and its diagnostics must match MATCH, so a case can't pass by
# failing for the wrong reason (a typo, a missing header, a missing SDK).
#
# A script rather than PASS_REGULAR_EXPRESSION on a bare compiler command
# (jaal's JaalCompileFail does that) because the control needs the opposite
# verdict from the same file, and because a failure here should say which
# way it went wrong instead of "regex did not match".

foreach(_v CXX STD SRC CASE)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "run_case.cmake: -D${_v}= is required")
    endif()
endforeach()

set(_cmd "${CXX}" "${STD}" -fsyntax-only "-DMAYA_CASE=${CASE}")
foreach(_dir IN LISTS INC)
    list(APPEND _cmd "-I${_dir}")
endforeach()
# The compiler is run by hand, so the SDK CMake would add for a real TU has
# to be added here: without it <optional> isn't found and every case "fails".
if(SYSROOT)
    list(APPEND _cmd -isysroot "${SYSROOT}")
endif()
list(APPEND _cmd "${SRC}")

execute_process(COMMAND ${_cmd}
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE  _out)

if(EXPECT_OK)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "case ${CASE} is the control and must compile:\n${_out}")
    endif()
    message(STATUS "case ${CASE}: compiles, as it must")
    return()
endif()

if(_rc EQUAL 0)
    message(FATAL_ERROR "case ${CASE} compiled; it must be rejected")
endif()
if(MATCH AND NOT _out MATCHES "${MATCH}")
    message(FATAL_ERROR
        "case ${CASE} failed, but not with /${MATCH}/ -- wrong reason:\n${_out}")
endif()
message(STATUS "case ${CASE}: rejected with /${MATCH}/")
