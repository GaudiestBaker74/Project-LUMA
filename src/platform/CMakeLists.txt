# =============================================================================
# Platform layer. The platform-specific implementation file is chosen here —
# this is the ONLY place where the host OS influences the build.
# =============================================================================

add_library(pc_platform STATIC
    platform.cpp
    Log/Log.cpp
    Memory/Memory.cpp
    Filesystem/Filesystem.cpp
    Timing/Timing.cpp
    Threading/Threading.cpp
)

if(PC_HOST_OS STREQUAL "linux")
    target_sources(pc_platform PRIVATE linux/PlatformLinux.cpp)
elseif(PC_HOST_OS STREQUAL "windows")
    target_sources(pc_platform PRIVATE windows/PlatformWindows.cpp)
endif()

# Include root is src/ so headers are included as "platform/...", "tests/...".
target_include_directories(pc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

pc_enable_warnings(pc_platform)

if(PC_HOST_OS STREQUAL "linux")
    # pthread for std::thread; atomic for std::atomic<Platform::Memory::Stats>
    # (a 48-byte struct, not lock-free — libatomic provides the helpers).
    target_link_libraries(pc_platform PRIVATE pthread atomic)
elseif(PC_HOST_OS STREQUAL "windows")
    # MinGW/GCC also needs libatomic for the non-lock-free atomic; MSVC ships
    # the helpers in its CRT, so only link it for non-MSVC Windows toolchains.
    if(NOT MSVC)
        target_link_libraries(pc_platform PRIVATE atomic)
    endif()
endif()
