cmake_minimum_required(VERSION 3.28)
project(maya VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release so out-of-the-box builds get -O3.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

add_library(maya INTERFACE)
target_include_directories(maya INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(maya INTERFACE cxx_std_26)

option(MAYA_BUILD_EXAMPLES "Build example programs" ON)
if(MAYA_BUILD_EXAMPLES)
    add_executable(maya_demo      examples/demo.cpp)
    add_executable(maya_counter   examples/counter.cpp)
    add_executable(maya_matrix    examples/matrix.cpp)
    add_executable(maya_viz       examples/viz.cpp)
    add_executable(maya_fireworks examples/fireworks.cpp)
    add_executable(maya_dashboard examples/dashboard.cpp)
    add_executable(maya_agent    examples/agent.cpp)
    add_executable(maya_progress examples/progress.cpp)

    foreach(tgt maya_demo maya_counter maya_matrix maya_viz maya_fireworks maya_dashboard maya_agent maya_progress)
        target_link_libraries(${tgt} PRIVATE maya)

        # Maximum optimizations for the examples.
        # -O3              — full optimization (also auto-vectorizes fill/diff loops)
        # -march=native    — use every instruction the local CPU supports
        # -funroll-loops   — unroll tight loops in the diff inner loop
        # -fno-rtti        — maya uses no RTTI; drop the overhead
        # LTO              — cross-TU inlining (let the linker merge headers)
        target_compile_options(${tgt} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:
                -O3
                -march=native
                -funroll-loops
            >
            $<$<CXX_COMPILER_ID:MSVC>:/O2 /GL>
        )
        set_target_properties(${tgt} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        )
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
    endforeach()
endif()

include(GNUInstallDirs)
install(DIRECTORY include/maya DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS maya EXPORT maya-targets)
install(EXPORT maya-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
    NAMESPACE maya::
)
