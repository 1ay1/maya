# fuzz/CMakeLists.txt — libFuzzer harnesses (Clang only).
#
# Each harness links libmaya and is compiled with -fsanitize=fuzzer plus
# address+undefined so a crash surfaces with a sanitizer report and a
# reproducer. Enabled via -DMAYA_BUILD_FUZZERS=ON from the top-level build.

set(MAYA_FUZZ_SANITIZE "fuzzer,address,undefined"
    CACHE STRING "Sanitizer set for fuzz harnesses")

set(MAYA_FUZZ_TARGETS
    fuzz_input
    fuzz_safe_break
    fuzz_osc52
    fuzz_markdown
)

foreach(name ${MAYA_FUZZ_TARGETS})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE maya)
    target_compile_options(${name} PRIVATE
        -g -O1 -fno-omit-frame-pointer
        -fsanitize=${MAYA_FUZZ_SANITIZE})
    target_link_options(${name} PRIVATE
        -fsanitize=${MAYA_FUZZ_SANITIZE})
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        CXX_SCAN_FOR_MODULES OFF)
endforeach()
