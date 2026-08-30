# =============================================================================
# Cross-compile to Windows x64 using MinGW-w64 (Linux host).
#
# This is a DEVELOPER convenience — it lets you verify the Windows build of
# the whole tree (platform/windows/PlatformWindows.cpp against real Win32
# headers) without a Windows machine. The official Windows build uses MSVC on
# windows-latest (see .github/workflows/ci.yml).
#
# Usage:
#   cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-MinGW.cmake -G Ninja
#   cmake --build build-mingw
#   # optional: run the tests under wine if installed
#   wine build-mingw/src/tests/galaxy-pc-tests.exe
# =============================================================================

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Cross-compilers must not accidentally pick up Linux host tools/libraries.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Keep the runtime self-contained (static libgcc/libstdc++/libwinpthread/
# libatomic — the std::atomic<Stats> helpers), so the exe runs under wine
# without a MinGW runtime DLL next to it. Only system DLLs (KERNEL32,
# msvcrt) remain as imports.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(PC_HOST_OS "windows" CACHE STRING "Forced by toolchain (MinGW targets Windows).")
