# =============================================================================
# Platform selection.
#
# The PC port is configured with -DPLATFORM=PC. For now that is the only
# supported target. The host OS is auto-detected and stored in PC_HOST_OS
# ("linux" or "windows"), which src/platform uses to pick the implementation
# directory (src/platform/linux vs src/platform/windows).
# =============================================================================

set(PLATFORM "PC" CACHE STRING "Build target platform (PC)")

if(NOT PLATFORM STREQUAL "PC")
    message(FATAL_ERROR
        "PLATFORM=${PLATFORM} is not supported yet. Only PLATFORM=PC is available.")
endif()

if(WIN32)
    set(PC_HOST_OS "windows")
elseif(UNIX AND NOT APPLE)
    set(PC_HOST_OS "linux")
elseif(APPLE)
    message(FATAL_ERROR "macOS is not supported yet (M1: Linux, M2: Windows).")
else()
    message(FATAL_ERROR "Unsupported host OS.")
endif()

message(STATUS "galaxy-pc: target=${PLATFORM} host=${PC_HOST_OS}")
