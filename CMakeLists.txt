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

# --- SDL3 (window/input/audio for the port) -----------------------------------
# Prefer the system package (Linux); fall back to building from source
# (Windows/MSVC and CI: FetchContent downloads the official release tarball).
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found on the system — fetching from source (FetchContent)")
    include(FetchContent)
    # Static build: no DLL to deploy. SDL3's default on Windows is shared-only,
    # so SDL_STATIC must be enabled explicitly (SDL_SHARED=OFF alone leaves
    # both off -> "SDL_SHARED and SDL_STATIC cannot both be disabled").
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    # Download the verified release tarball instead of `git clone`. The git
    # path is fragile on Windows: it leaves read-only .git objects and its
    # "remove sdl3-src before re-cloning" step fails whenever OneDrive (the
    # project is often under Downloads), antivirus, or a stray git.exe holds a
    # file lock — the exact "Failed to remove directory ... sdl3-src" error.
    # A single hash-verified .tar.gz avoids all of that and is faster.
    # FetchContent flattens the archive's single top-level directory, so
    # SDL3's CMakeLists.txt lands at the source root (no SOURCE_SUBDIR needed).
    FetchContent_Declare(
        sdl3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.tar.gz
        URL_HASH SHA256=f87be7b4dec66db4098e9c167b2aa34e2ca10aeb5443bdde95ae03185ed513e0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sdl3)
endif()

# --- Vulkan headers (the loader is NOT linked: volk loads it at runtime) -----
# find_package(Vulkan) locates the SDK (headers + loader). The loader is only
# *found*, never linked — volk resolves every entry point at runtime. There is
# no "headers" COMPONENT in FindVulkan (only glslc/glslang/shaderc/...), so we
# use the plain package and only consume Vulkan_INCLUDE_DIRS.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR
        "Vulkan not found.\n"
        "  - Windows: install the LunarG Vulkan SDK from "
        "https://vulkan.lunarg.com/sdk/home#windows and reopen your "
        "terminal/IDE so the VULKAN_SDK environment variable is set.\n"
        "  - Linux:   sudo apt install libvulkan-dev\n"
        "  - macOS:   brew install vulkan-headers")
endif()

add_subdirectory(src)
