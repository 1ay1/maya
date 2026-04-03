cmake_minimum_required(VERSION 3.28)
project(maya VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(maya INTERFACE)
target_include_directories(maya INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(maya INTERFACE cxx_std_26)

option(MAYA_BUILD_EXAMPLES "Build example programs" ON)
if(MAYA_BUILD_EXAMPLES)
    add_executable(maya_demo examples/demo.cpp)
    target_link_libraries(maya_demo PRIVATE maya)

    add_executable(maya_counter examples/counter.cpp)
    target_link_libraries(maya_counter PRIVATE maya)
endif()

option(MAYA_BUILD_TESTS "Build tests" OFF)
if(MAYA_BUILD_TESTS)
    enable_testing()
    add_executable(maya_static_tests tests/static_tests.cpp)
    target_link_libraries(maya_static_tests PRIVATE maya)
    add_test(NAME static_tests COMMAND maya_static_tests)
endif()

include(GNUInstallDirs)
install(DIRECTORY include/maya DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS maya EXPORT maya-targets)
install(EXPORT maya-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/maya
    NAMESPACE maya::
)
