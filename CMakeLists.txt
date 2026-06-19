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
#
# MAYA_NATIVE_TUNING gates -march=native / -mtune=native. Default ON for
# local dev (the producing host == the running host), but consumers shipping
# a portable binary MUST turn it off — `-march=native` bakes the build
# host's specific microarchitecture into the binary, and the result will
# SIGILL on any chip lacking those features. Particularly visible on
# aarch64, where CI runners (Graviton2/3/4) emit instructions older
# Cortex-A cores can't decode. With this OFF the build relies on the
# compiler's default baseline (`-march=x86-64` / `armv8-a`) which every
# chip of the relevant arch supports.
option(MAYA_NATIVE_TUNING "Bake the host CPU into the binary (-march=native -mtune=native). OFF for portable / standalone releases." ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # LTO across all translation units — lets GCC inline across .cpp boundaries,
    # devirtualize, and eliminate dead code globally. Respect a caller-supplied
    # value (e.g. distro packagers pass -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
    # so the installed libmaya.a contains real object code, not LTO bytecode
    # tied to the producing GCC version).
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()

    if(MAYA_NATIVE_TUNING)
        set(MAYA_ARCH_FLAGS -march=native -mtune=native)
    else()
        set(MAYA_ARCH_FLAGS)   # rely on the compiler's default arch baseline
    endif()

    # Shared flags for library and examples
    set(MAYA_LINUX_CXX_FLAGS
        -O3
        ${MAYA_ARCH_FLAGS}           # native tuning gated on MAYA_NATIVE_TUNING
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
    src/render/frame_bytes.cpp
    src/render/inline_frame.cpp
    src/render/canvas_witness.cpp
    src/widget/markdown.cpp
    # markdown widget — three modules: parser / render / streaming
    src/widget/markdown/parser/parser_inline.cpp
    src/widget/markdown/parser/parser_block.cpp
    src/widget/markdown/parser/text_transform.cpp
    # CommonMark engine — spec-faithful two-phase core
    src/widget/markdown/engine/cm_block.cpp
    src/widget/markdown/engine/cm_inline.cpp
    src/widget/markdown/render/render_inline.cpp
    src/widget/markdown/render/render_block.cpp
    src/widget/markdown/render/syntax_lang.cpp
    src/widget/markdown/render/syntax_highlight.cpp
    # streaming module — one concern per TU
    src/widget/markdown/streaming/markdown_memo.cpp
    src/widget/markdown/streaming/boundary.cpp
    src/widget/markdown/streaming/commit.cpp
    src/widget/markdown/streaming/render_tail.cpp
    src/widget/markdown/streaming/build.cpp
    src/widget/markdown/streaming/reveal_fx.cpp
    src/widget/markdown/streaming/folding.cpp
    src/widget/markdown/streaming/async.cpp

    src/widget/html/tags.cpp
    src/widget/html/tokenizer.cpp
    src/widget/html/parser.cpp
    src/widget/html/render.cpp
)
target_include_directories(maya PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ── Unicode width table regeneration (manual target) ─────────────────────
# `cmake --build build --target maya_unicode_regen` re-runs the generator
# script over the pinned UCD files in maya/data/ and overwrites
# include/maya/text/unicode_width_table.hpp. The generated header is
# checked in, so the regular build never needs Python — this target is
# only for bumping to a new Unicode revision (drop newer .txt files into
# maya/data/, then run this target, then commit the new .hpp).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(maya_unicode_regen
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_unicode_width.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        BYPRODUCTS ${CMAKE_CURRENT_SOURCE_DIR}/include/maya/text/unicode_width_table.hpp
        COMMENT "Regenerating maya/include/maya/text/unicode_width_table.hpp from pinned UCD"
        VERBATIM
    )
endif()
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
            $<$<BOOL:${MAYA_NATIVE_TUNING}>:-march=native>
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
                    $<$<BOOL:${MAYA_NATIVE_TUNING}>:-march=native>
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
        tests/test_render_scaling.cpp
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
        tests/test_widgets.cpp
        tests/test_promoted.cpp
        tests/test_markdown.cpp
        tests/test_streaming_cache.cpp
        tests/test_tracked.cpp
        tests/test_unicode_width.cpp
        tests/test_stream_sink.cpp
        tests/test_scroll.cpp
        tests/test_wire_row.cpp
        tests/test_inline_frame.cpp
        tests/test_commonmark_spec.cpp
        tests/test_html.cpp
        tests/bench_thread_scaling.cpp
    )

    foreach(src ${MAYA_TEST_SOURCES})
        get_filename_component(tgt ${src} NAME_WE)
        add_executable(${tgt} ${src})
        target_link_libraries(${tgt} PRIVATE maya)
        target_compile_definitions(${tgt} PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_options(${tgt} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:
                -O2
                $<$<BOOL:${MAYA_NATIVE_TUNING}>:-march=native>
            >
        )
        add_test(NAME ${tgt} COMMAND ${tgt})
        set_tests_properties(${tgt} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

# ── Fuzzers (libFuzzer; Clang only) ────────────────────────────────────────
# Harnesses for the byte-parsing surfaces most exposed to adversarial input:
# the terminal input parser, the writer's safe-break splitter, the OSC-52
# base64 path, and the markdown engine. Build with Clang and a fuzzer
# sanitizer; run via CI (.github/workflows/ci.yml fuzz-smoke job) or a
# scheduled long run.
#
#   CC=clang CXX=clang++ cmake -B fuzzbuild -DMAYA_BUILD_FUZZERS=ON
#   cmake --build fuzzbuild -j
#   ./fuzzbuild/fuzz/fuzz_input -max_total_time=60
option(MAYA_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only)" OFF)
if(MAYA_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MAYA_BUILD_FUZZERS requires Clang (libFuzzer).")
    endif()
    add_subdirectory(fuzz)
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
