# =============================================================================
# Cross-compile to Linux aarch64 (Steam Deck / ARM64 hosts).
#
# Verifies the whole tree compiles for aarch64. Executables are not runnable
# on an x86-64 host — this is a compile-only check (see the CI job
# "linux-aarch64-cross").
#
# Usage:
#   sudo apt-get install -y g++-aarch64-linux-gnu
#   cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-LinuxAArch64.cmake -G Ninja -DPLATFORM=PC
#   cmake --build build-arm64
# =============================================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Cross-compilers must not pick up x86-64 host tools/libraries.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
