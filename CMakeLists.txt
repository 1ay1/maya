cmake_minimum_required(VERSION 3.28)
project(maya VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release so out-of-the-box builds get -O3.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── Linux-specific aggressive optimization ────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # LTO across all translation units — lets GCC inline across .cpp boundaries,
    # devirtualize, and eliminate dead code globally.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

    # Shared flags for library and examples
    set(MAYA_LINUX_CXX_FLAGS
        -O3
        -march=native                # AVX2/BMI2/FMA on the host CPU
        -mtune=native                # schedule for host microarchitecture
        -funroll-loops               # unroll small loops
        -ftree-vectorize             # auto-vectorize where SIMD fits
        -fprefetch-loop-arrays       # insert prefetch for sequential access
        -fomit-frame-pointer         # free up RBP as GPR
        -fno-plt                     # direct calls through GOT, skip PLT indirection
        -fno-semantic-interposition  # assume symbols won't be interposed at runtime
        -fvisibility=hidden          # hide all symbols by default, reduce PLT/GOT
        -fvisibility-inlines-hidden  # hide inline function bodies
        -ffunction-sections          # each function in its own section for --gc-sections
        -fdata-sections              # each global in its own section
        -fmerge-all-constants        # merge identical constants across TUs
        -fdevirtualize-speculatively # speculative devirt for indirect calls
        -fipa-pta                    # interprocedural pointer analysis
        -fgraphite-identity          # enable Graphite loop optimizations
        -floop-nest-optimize         # Graphite polyhedral loop nest optimization
        -ftree-loop-distribution     # distribute loops for better vectorization
        -fno-math-errno              # don't set errno on math functions
        -fno-trapping-math           # assume no FP traps
        -fno-stack-protector         # no stack canaries — TUI app, not a network daemon
    )

    set(MAYA_LINUX_LINK_FLAGS
        -Wl,--gc-sections            # discard unreferenced sections
        -Wl,--as-needed              # only link actually-used libraries
        -Wl,-O2                      # linker optimization level
        -Wl,--hash-style=gnu         # faster symbol lookup
        -Wl,--sort-common            # sort common symbols by alignment
        -Wl,-z,now                   # resolve all symbols at load time (RELRO)
        -Wl,-z,relro                 # read-only relocations
    )
endif()

add_library(maya
    src/app/app.cpp
    src/app/events.cpp
    src/app/inline.cpp
    src/element/builder.cpp
    src/element/text.cpp
    src/terminal/ansi.cpp
    src/terminal/input.cpp
    src/terminal/writer.cpp
    src/layout/yoga.cpp
    src/render/canvas.cpp
    src/render/renderer.cpp
    src/render/diff.cpp
    src/render/serialize.cpp
    src/widget/markdown.cpp
)
target_include_directories(maya PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(MSVC)
    # CMake has no /std:c++26 for MSVC yet. Publicly require C++23 and
    # opt PRIVATEly into /std:c++latest — consumers linking maya stay
    # on C++23 but maya's own TUs get the newer library surface.
    target_compile_features(maya PUBLIC cxx_std_23)
    # Force-include the platform detect header so __builtin_unreachable /
    # __builtin_expect polyfills are visible to every maya TU AND every
    # downstream consumer (propagated via PUBLIC /FI).
    target_compile_options(maya PUBLIC
        /FI${CMAKE_CURRENT_SOURCE_DIR}/include/maya/platform/detect.hpp
    )
else()
    target_compile_features(maya PUBLIC cxx_std_26)
endif()

if(DEFINED MAYA_LINUX_CXX_FLAGS)
    target_compile_options(maya PRIVATE ${MAYA_LINUX_CXX_FLAGS})
    target_link_options(maya PRIVATE ${MAYA_LINUX_LINK_FLAGS})
else()
    target_compile_options(maya PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -O3
            -march=native
            -funroll-loops
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /utf-8
            /std:c++latest
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
            /Zc:inline
            /Zc:throwingNew
            /EHsc
            /bigobj
            /MP
            /wd4100
            /wd4127
            /wd4324
            /wd4244  # conversion, possible loss of data
            /wd4267  # size_t -> int
        >
        # ── MSVC Release-only aggressive optimization ─────────────────
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:
            /O2
            /Ob3               # aggressive inlining
            /Oi                # intrinsics
            /Ot                # favor speed over size
            /Oy                # omit frame pointer
            /GL                # whole-program optimization
            /GF                # string pooling
            /Gy                # function-level linking
            /Gw                # data-level linking
            /GS-               # disable stack cookies (TUI, not a daemon)
            /fp:fast           # relax FP — fine for layout math
            /arch:AVX2         # baseline AVX2/BMI2/FMA (consistent with SIMD caps)
            /Qpar              # auto-parallelizer for hot loops
        >
    )
    target_link_options(maya PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:
            /LTCG
            /OPT:REF
            /OPT:ICF=3
            /INCREMENTAL:NO
            /DEBUG:NONE
        >
    )
    if(MSVC)
        target_compile_definitions(maya PUBLIC
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>
        )
    endif()
endif()

option(MAYA_BUILD_EXAMPLES "Build example programs" ON)
if(MAYA_BUILD_EXAMPLES)
    file(GLOB MAYA_EXAMPLE_SOURCES examples/*.cpp)
    foreach(src ${MAYA_EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        set(tgt maya_${name})
        add_executable(${tgt} ${src})
        target_link_libraries(${tgt} PRIVATE maya)
        if(DEFINED MAYA_LINUX_CXX_FLAGS)
            target_compile_options(${tgt} PRIVATE ${MAYA_LINUX_CXX_FLAGS})
            target_link_options(${tgt} PRIVATE ${MAYA_LINUX_LINK_FLAGS})
        else()
            target_compile_options(${tgt} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:
                    -O3
                    -march=native
                    -funroll-loops
                >
                $<$<CXX_COMPILER_ID:MSVC>:/O2 /GL>
            )
        endif()
    endforeach()
endif()

option(MAYA_BUILD_TESTS "Build tests" OFF)
if(MAYA_BUILD_TESTS)
    enable_testing()

    set(MAYA_TEST_SOURCES
        tests/test_render.cpp
        tests/test_types.cpp
        tests/test_color.cpp
        tests/test_style.cpp
        tests/test_canvas.cpp
        tests/test_diff.cpp
        tests/test_framebuffer.cpp
        tests/test_input.cpp
        tests/test_signal.cpp
        tests/test_layout.cpp
        tests/test_border.cpp
        tests/test_simd.cpp
        tests/test_ansi.cpp
        tests/test_writer.cpp
        tests/test_resize.cpp
        tests/test_widgets.cpp
        tests/test_promoted.cpp
        tests/test_markdown.cpp
    )

    foreach(src ${MAYA_TEST_SOURCES})
        get_filename_component(tgt ${src} NAME_WE)
        add_executable(${tgt} ${src})
        target_link_libraries(${tgt} PRIVATE maya)
        target_compile_options(${tgt} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:
                -O2 -march=native
            >
        )
        add_test(NAME ${tgt} COMMAND ${tgt})
        set_tests_properties(${tgt} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/maya DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS maya EXPORT maya-targets)
install(EXPORT maya-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
    NAMESPACE maya::
)

# Generate maya-config.cmake so downstream projects can find_package(maya)
configure_package_config_file(
    cmake/maya-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
)

# Generate maya-config-version.cmake for version compatibility
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/maya-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
)
