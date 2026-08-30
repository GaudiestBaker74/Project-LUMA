# =============================================================================
# Wii compatibility layer (src/compat) + the first native-compiled Petari
# module (JSystem JKernel heaps) — the M1 smoke test.
#
# Headers: the game code keeps using the ORIGINAL Petari headers. Our patched
# copies (src/compat/include) are added FIRST in the include path and override
# the upstream ones for the few files that cannot compile as-is on a host
# toolchain (see compat/include/README.md).
# =============================================================================

set(PETARI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/petari)

# JKRHeap.cpp and JKRExpHeap.cpp are compiled from src/compat/patches
# (patched copies of the vendored originals) — see patches/README.md for why.
set(JKR_HEAP_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRHeap.cpp)
set(JKR_EXPHEAP_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRExpHeap.cpp)

add_library(pc_compat STATIC
    os/OSArena.cpp
    os/OSMutex.cpp
    os/OSReport.cpp
    jsystem/JUTConsoleCompat.cpp
    jsystem/JUTExceptionCompat.cpp
    # M3 "first GX smell": minimal GX subset routed to Platform::Video.
    # Replaced by the real GX implementation in M5 (see gx/GXCompat.h).
    gx/GXCompat.cpp
    # --- Vendored Petari game code (unmodified or patched) --------------------
    ${JKR_EXPHEAP_SOURCE}   # patched copy (see patches/README.md)
    ${JKR_HEAP_SOURCE}      # patched copy (see patches/README.md)
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRDisposer.cpp
    # JSUPtrList/JSUPtrLink (used by JKRHeap's heap registry). Compiles as-is.
    ${PETARI_ROOT}/src/JSystem/JSupport/JSUList.cpp
)

target_include_directories(pc_compat PUBLIC
    # Patched overrides FIRST (win over the vendored headers below).
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Source root so game/compat code includes "compat/...", "platform/...".
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    # JSystem + nw4r headers.
    ${PETARI_ROOT}/libs/JSystem/include
    # Revolution SDK headers (types, OS, GX...).
    ${PETARI_ROOT}/libs/RVL_SDK/include
    # Game headers (src/Game includes "Game/...") — added when game code is
    # compiled (later milestones).
)

target_link_libraries(pc_compat PUBLIC pc_platform)

# The vendored Petari sources are old-style decompiled code: silence warnings
# for them (see cmake/CompilerWarnings.cmake). Flag differs per toolchain:
# GCC/Clang use -w, MSVC uses /w.
set(PETARI_SOURCES
    ${JKR_EXPHEAP_SOURCE}
    ${JKR_HEAP_SOURCE}
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRDisposer.cpp
    ${PETARI_ROOT}/src/JSystem/JSupport/JSUList.cpp
)
set_source_files_properties(${PETARI_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/w>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")
