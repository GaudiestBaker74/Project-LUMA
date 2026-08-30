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
    Window/Window.cpp
    Input/Input.cpp
    Renderer/Renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/volk/volk.c
)

if(PC_HOST_OS STREQUAL "linux")
    target_sources(pc_platform PRIVATE linux/PlatformLinux.cpp)
elseif(PC_HOST_OS STREQUAL "windows")
    target_sources(pc_platform PRIVATE windows/PlatformWindows.cpp)
endif()

# Include root is src/ so headers are included as "platform/...", "tests/...".
target_include_directories(pc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# volk (Vulkan meta-loader) — header + implementation live in third_party/volk.
target_include_directories(pc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/volk)
# Vulkan SDK headers (loader intentionally NOT linked — volk loads it at runtime).
target_include_directories(pc_platform PUBLIC ${Vulkan_INCLUDE_DIRS})

pc_enable_warnings(pc_platform)

# SDL3 is PUBLIC: consumers (galaxy-pc, galaxy-pc-tests) include SDL3 headers
# indirectly via platform/Window/Window.h and need the target too.
if(TARGET SDL3::SDL3)
    target_link_libraries(pc_platform PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-shared)
    target_link_libraries(pc_platform PUBLIC SDL3::SDL3-shared)
else()
    message(FATAL_ERROR "SDL3 target not found (system or FetchContent)")
endif()

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
