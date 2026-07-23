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

# Treat warnings as errors. OFF by default so day-to-day dev builds aren't
# blocked by a stray warning; CI turns it ON (-DMAYA_WERROR=ON) to keep the
# tree warning-clean. Applies -Werror (GCC/Clang) or /WX (MSVC) to the maya
# target; -Wall -Wextra / -W4 are always on regardless of this flag.
option(MAYA_WERROR "Treat compiler warnings as errors (CI)." OFF)

# ── Fast test iteration ───────────────────────────────────────────────────
# The production `maya` library compiles every TU at -O3 with LTO + loop
# unrolling + Graphite polyhedral passes (see MAYA_LINUX_CXX_FLAGS). Great for
# the shipped binary, terrible for the test edit-build-run loop: any header
# edit recompiles the whole library at -O3, and every test then LTO-links
# (whole-program optimization) against it.
#
# With MAYA_FAST_TESTS=ON the tests link a SEPARATE `maya_test` library built
# at -O0 with NO LTO and NO heavy flags, and test TUs compile at -O0 too. A
# header edit recompiles only the touched object at -O0 and links instantly.
# The production `maya` target and its flags are completely untouched, so the
# portable-release path is unaffected. Default ON whenever tests are enabled.
option(MAYA_FAST_TESTS "Build tests against a non-LTO, -O0 maya library for fast iteration." ON)

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
elseif(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # macOS + GCC (Homebrew g++). Same intent as the Linux block — wring
    # the rendering hot paths — but Mach-O, not ELF, so the flag set is the
    # subset that's meaningful here. NO LTO (Apple ld can't consume GCC LTO
    # IR; agentty's top-level CMake already forces IPO off for this combo).
    #
    # No -march/-mtune=native: Homebrew GCC on Apple Silicon resolves
    # `native` to `apple-m1`, which its aarch64 backend doesn't accept as an
    # -march value ("unknown value 'apple-m1'"), so it hard-errors the
    # compile. We fall back to GCC's armv8-a baseline — which still has NEON,
    # so -ftree-vectorize keeps emitting SIMD; we just forgo CPU-specific
    # instruction scheduling. (This is a GCC/Apple-Silicon gap, not a choice.)

    # Excluded vs the Linux set, on purpose:
    #   -fno-plt / -fno-semantic-interposition — ELF symbol-interposition
    #     concepts with no Mach-O meaning (GCC warns or silently ignores).
    #   -fgraphite-identity / -floop-nest-optimize — polyhedral passes are
    #     the least-exercised on macOS GCC; the marginal gain isn't worth
    #     the correctness risk on a TUI's render loop. Kept Linux-only.
    set(MAYA_MACOS_CXX_FLAGS
        -O3
        -funroll-loops
        -ftree-vectorize             # auto-vectorize (NEON on arm64)
        -fprefetch-loop-arrays
        -fomit-frame-pointer
        -fvisibility=hidden          # proven safe on the Linux static-link path
        -fvisibility-inlines-hidden
        -ffunction-sections          # pairs with -dead_strip below
        -fdata-sections
        -fmerge-all-constants
        -fdevirtualize-speculatively
        -fipa-pta                    # interprocedural pointer analysis
        -ftree-loop-distribution
        -fno-math-errno
        -fno-trapping-math
        -fno-stack-protector         # TUI app, not a network daemon
    )

    # Apple's ld doesn't speak GNU-ld flags (--gc-sections / --as-needed /
    # -z now / --hash-style all error out). The one equivalent that matters
    # for the -ffunction-sections/-fdata-sections pairing is -dead_strip.
    set(MAYA_MACOS_LINK_FLAGS
        -Wl,-dead_strip              # discard unreferenced sections (≈ --gc-sections)
    )
endif()

set(MAYA_LIB_SOURCES
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
    src/render/scrollback_ledger.cpp
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
    src/widget/markdown/render/highlight.cpp
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
    src/widget/html/css.cpp
    src/widget/html/render.cpp
)

add_library(maya ${MAYA_LIB_SOURCES})
# maya uses no C++20/23 modules (no `export module` / `import` anywhere). With
# CMAKE_CXX_STANDARD 26 and CMake's ninja generator, module dependency scanning
# is enabled by default — which here is pure overhead AND incorrect: the html/*
# TUs' .ddi scan kept them perpetually dirty and reordered their compile to
# AFTER the libmaya.a archive link, so a parallel build could archive stale
# objects or read the archive mid-write (corrupt member -> link failures /
# random std::system_error at runtime). Turning scanning off makes the build
# deterministic and order-correct.
set_target_properties(maya PROPERTIES CXX_SCAN_FOR_MODULES OFF)
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
        COMMAND ${CMAKE_COMMAND}
            -E copy_if_different
             ${CMAKE_CURRENT_SOURCE_DIR}/include/maya/text/unicode_width_table.hpp.tmp
             ${CMAKE_CURRENT_SOURCE_DIR}/include/maya/text/unicode_width_table.hpp
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
elseif(DEFINED MAYA_MACOS_CXX_FLAGS)
    target_compile_options(maya PRIVATE ${MAYA_MACOS_CXX_FLAGS})
    target_link_options(maya PRIVATE ${MAYA_MACOS_LINK_FLAGS})
else()
    target_compile_options(maya PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -O3
            $<$<BOOL:${MAYA_NATIVE_TUNING}>:-march=native>
            -funroll-loops
            # Warnings. This code is SIMD- and pointer-heavy; -Wall -Wextra
            # is exactly the net that catches the class of bug (unaligned
            # intrinsics, sign/size mismatches) that has bitten this file
            # before. Opt into -Werror via -DMAYA_WERROR=ON in CI only — the
            # default keeps warnings visible without breaking dev builds.
            -Wall
            -Wextra
            # Partial designated-init (`TextElement{.content=…}`) leaves cache
            # fields (cells / cached_lines / kids / cached_render) default-
            # initialized on purpose; unused overload params, dead locals
            # behind #if'd paths, and default-fallthrough switches are all
            # intentional. These are benign under Clang's stricter -Wextra
            # net — silence them so a dev build is warning-clean. CI still
            # gets the full net via -DMAYA_WERROR=ON (which does NOT inherit
            # these, since they're appended after the WERROR generator expr
            # only when Werror is off would be ideal; here we keep them on
            # both, treating the class as known-benign project-wide).
            -Wno-missing-field-initializers
            -Wno-unused-parameter
            -Wno-unused-variable
            -Wno-unused-function
            -Wno-unused-but-set-variable
            -Wno-switch
            $<$<CXX_COMPILER_ID:Clang>:-Wno-missing-designated-field-initializers>
            $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-private-field>
            $<$<BOOL:${MAYA_WERROR}>:-Werror>
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
            /W4                # match -Wall -Wextra
            $<$<BOOL:${MAYA_WERROR}>:/WX>
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

    # ── Which library do tests link against? ──────────────────────────────
    # MAYA_FAST_TESTS builds a parallel `maya_test` library from the SAME
    # sources but with LTO disabled and -O0 — so a header edit recompiles
    # only the touched .o at -O0 and the test links in milliseconds instead
    # of waiting on whole-program LTO. Production `maya` is left alone.
    if(MAYA_FAST_TESTS AND NOT MSVC)
        add_library(maya_test STATIC EXCLUDE_FROM_ALL ${MAYA_LIB_SOURCES})
        set_target_properties(maya_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        target_include_directories(maya_test PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
        target_compile_features(maya_test PUBLIC cxx_std_26)
        # No LTO, no -O3, no unroll/Graphite. Just enough to be correct + fast
        # to compile. -g0 keeps objects small; debugging a failing test is
        # better done by rebuilding the one TU with -Og if needed.
        set_target_properties(maya_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        # -O1 (NOT -O0): a handful of tests assert per-frame render latency
        # (test_markdown's streaming cases HUNG at -O0). -O1 has no LTO, no
        # loop unrolling, no Graphite polyhedral passes — so it compiles in a
        # fraction of the production -O3+LTO time — yet runs fast enough for
        # those gates. Native tuning is intentionally omitted (portability +
        # compile speed). Test TUs themselves stay at -O0 (see MAYA_TEST_OPT).
        target_compile_options(maya_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-O1 -g0 -fno-lto>)
        target_link_options(maya_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-lto>)
        set(MAYA_TEST_LIB maya_test)
        set(MAYA_TEST_OPT -O0 -g0 -fno-lto)
    else()
        set(MAYA_TEST_LIB maya)
        set(MAYA_TEST_OPT -O2)
        if(MAYA_NATIVE_TUNING)
            list(APPEND MAYA_TEST_OPT -march=native)
        endif()
    endif()

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
        tests/test_animation.cpp
        tests/test_motion.cpp
        tests/test_overlay_layer.cpp
        tests/test_static_split.cpp
        tests/test_highlight.cpp
        tests/test_highlight_resume.cpp
        tests/test_layout.cpp
        tests/test_border.cpp
        tests/test_simd.cpp
        tests/test_ansi.cpp
        tests/test_writer.cpp
        tests/test_widgets.cpp
        tests/test_promoted.cpp
        tests/test_markdown.cpp
        tests/test_streaming_cache.cpp
        tests/test_streaming_window.cpp
        tests/test_tracked.cpp
        tests/test_unicode_width.cpp
        tests/test_stream_sink.cpp
        tests/test_scroll.cpp
        tests/test_wire_row.cpp
        tests/test_commonmark_spec.cpp
        tests/test_html.cpp
        tests/bench_thread_scaling.cpp
    )

    foreach(src ${MAYA_TEST_SOURCES})
        get_filename_component(tgt ${src} NAME_WE)
        add_executable(${tgt} ${src})
        target_link_libraries(${tgt} PRIVATE ${MAYA_TEST_LIB})
        target_compile_definitions(${tgt} PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_options(${tgt} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:${MAYA_TEST_OPT}>)
        add_test(NAME ${tgt} COMMAND ${tgt})
        set_tests_properties(${tgt} PROPERTIES TIMEOUT 120)
    endforeach()

    # POSIX-only tests. These mock a real fd (a pipe / pty), so they need
    # unistd/termios and can't build under MSVC.
    #   • test_inline_frame — routes the Witness Chain through a pipe Writer;
    #     builds on Linux + macOS.
    #   • test_terminal_restore — PTY-driven; verifies maya puts the tty back
    #     (cooked termios + restore escapes) on every exit route, including
    #     SIGINT. Linux-only: it drives POSIX signal/pty surface the macOS
    #     toolchain doesn't expose cleanly from this TU, and the restore
    #     behaviour it guards is platform-identical.
    if(UNIX)
        set(_maya_posix_tests test_inline_frame test_fresh_oversized_scrollback)
        if(NOT APPLE)
            list(APPEND _maya_posix_tests test_terminal_restore)
        endif()
        foreach(tgt ${_maya_posix_tests})
            add_executable(${tgt} tests/${tgt}.cpp)
            target_link_libraries(${tgt} PRIVATE ${MAYA_TEST_LIB})
            target_compile_definitions(${tgt} PRIVATE
                MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
            target_compile_options(${tgt} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:${MAYA_TEST_OPT}>)
            add_test(NAME ${tgt} COMMAND ${tgt})
            set_tests_properties(${tgt} PROPERTIES TIMEOUT 120)
        endforeach()
        # forkpty() needs -lutil on Linux/glibc.
        if(NOT APPLE)
            target_link_libraries(test_terminal_restore PRIVATE util)
        endif()
    endif()
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
