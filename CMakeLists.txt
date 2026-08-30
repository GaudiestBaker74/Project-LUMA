# =============================================================================
# galaxy-pc — native PC port of Super Mario Galaxy 1 (based on Petari).
#
#   cmake -B build -DPLATFORM=PC
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# The decompilation build (third_party/petari/configure.py + ninja) is
# untouched and independent; this is the separate native PC build.
# =============================================================================

cmake_minimum_required(VERSION 3.20)

project(galaxy-pc VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/PlatformSelect.cmake)
include(cmake/CompilerWarnings.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|MSVC")
    message(WARNING "Untested compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# MSVC: the CRT deprecation warnings (C4996) for the classic C functions used
# by the ported game code (getenv, fopen, ...) are noise — they are perfectly
# safe to use here. Silence them globally instead of touching game code.
# /EHsc enables C++ exception unwinding semantics (the standard library uses
# exceptions in <chrono>/<thread> etc.); without it MSVC warns C4530.
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/EHsc)
endif()

# Must run before add_subdirectory() so the add_test() calls in the
# subdirectories register with ctest.
enable_testing()

add_subdirectory(src)
