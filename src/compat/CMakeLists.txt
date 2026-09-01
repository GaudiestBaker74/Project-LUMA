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
    os/PPCIntrinsics.cpp  # __cntlzw etc. (PPC intrinsics used by vendored inline code)
    jsystem/JUTConsoleCompat.cpp
    jsystem/JUTExceptionCompat.cpp
    # M3 "first GX smell": minimal GX subset routed to Platform::Video.
    # Replaced by the real GX implementation in M5 (see gx/GXCompat.h).
    gx/GXCompat.cpp
    gx/Bti.cpp
    gx/GXTexture.cpp
    gx/GXTev.cpp
    # M5.7c: EFB copy — GXCopyDisp/GXCopyTex + the copy-state APIs (render
    # target + blit/present, see docs/gx.md §J).
    gx/GXCopy.cpp
    # M7 (M7.1+M7.2): DVD compat — FST from the assets tree + synchronous
    # reads + the async worker queue (docs/assets.md).
    dvd/DVD.cpp
    # M5.7a: channel lighting + pos/nrm matrices (the GXInitLight* builders
    # come from the patched GXLight.c below).
    gx/GXLight.cpp
    # M5.7b: indirect (bump) texturing stages — mirror + UBO packing + the
    # GXSetNumIndStages/GXSetIndTex*/GXSetTevIndirect/GXSetTevIndWarp APIs.
    gx/GXInd.cpp
    # M6: input compat — the KPAD/WPAD APIs the game calls, over the raw
    # Platform::Input snapshot (docs/input.md §2).
    kpad/KPAD.cpp
    kpad/InputConfig.cpp
    wpad/WPAD.cpp
    # M6.5: host shims for Petari's Metrowerks math headers (see
    # compat/include/JSystem/JGeometry & JMath + docs/input.md §5.3):
    # JMath tables come from the vendored JMATrigonometric.cpp (clean);
    # JMathCompat.cpp fills the non-inline JMathInlineVEC::PSVEC* that the
    # PPC build provided via asm (JMath.cpp is __MWERKS__-only and not
    # compiled on the host).
    ${PETARI_ROOT}/src/JSystem/JMath/JMATrigonometric.cpp
    jsystem/JMathCompat.cpp
    # M6.5: Game/System WPad wrappers (the real game code) — see the comment
    # above SystemCompat.cpp for what is provided host-side and why.
    game/SystemCompat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gx/GXLight.c
    # --- Vendored Petari game code (unmodified or patched) --------------------
    ${JKR_EXPHEAP_SOURCE}   # patched copy (see patches/README.md)
    ${JKR_HEAP_SOURCE}      # patched copy (see patches/README.md)
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRDisposer.cpp
    # JSUPtrList/JSUPtrLink (used by JKRHeap's heap registry). Compiles as-is.
    ${PETARI_ROOT}/src/JSystem/JSupport/JSUList.cpp
    # M5.6: GD display-list recording primitives (patched copy — the only
    # change is the DCFlushRange no-op, see patches/RVL_SDK/gd/GDBase.c).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gd/GDBase.c
    # M5.7a: the GXInitLight* light-object builders (pure struct writes; the
    # HW-writing GX functions were removed — see patches/RVL_SDK/gx/GXLight.c).
    # The .c suffix keeps the "patched vendored file" story, but the PC GX
    # header overrides (GXVert.h -> GXCompatFifo.h) are C++, so compile it as
    # C++ (the bodies are plain C and compile unchanged).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gx/GXLight.c
    # M6.5: the Game/System WPad wrappers (real game code). Compile-only
    # integration: these objects only get pulled by the linker when game code
    # references them (static lib semantics), so WPadAcceleration.cpp — whose
    # updateAccAverage/updateRotate are still unmatched in the decomp — does
    # not block the build. See src/compat/game/SystemCompat.cpp.
    ${PETARI_ROOT}/src/Game/System/WPadButton.cpp
    ${PETARI_ROOT}/src/Game/System/WPadStick.cpp
    ${PETARI_ROOT}/src/Game/System/WPadAcceleration.cpp
    ${PETARI_ROOT}/src/Game/System/WPadRumble.cpp
    ${PETARI_ROOT}/src/Game/System/WPadRumbleData.cpp
    ${PETARI_ROOT}/src/Game/System/WPadInfoChecker.cpp
    ${PETARI_ROOT}/src/Game/System/WPadLeaveWatcher.cpp
)
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gx/GXLight.c
    PROPERTIES LANGUAGE CXX)

target_include_directories(pc_compat PUBLIC
    # Patched overrides FIRST (win over the vendored headers below).
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Source root so game/compat code includes "compat/...", "platform/...".
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    # JSystem + nw4r headers.
    ${PETARI_ROOT}/libs/JSystem/include
    # Revolution SDK headers (types, OS, GX...).
    ${PETARI_ROOT}/libs/RVL_SDK/include
    # Game headers ("Game/...") — needed by the M6.5 WPad wrapper integration
    # (and, later, all of Game/System).
    ${PETARI_ROOT}/include
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
    # M5.6: the GD display-list recording primitives (patched copy).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gd/GDBase.c
    # M5.7a: the GXInitLight* light-object builders (patched copy).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gx/GXLight.c
    # M6.5: JMath trig tables (vendored, clean) + the Game/System WPad
    # wrappers (real game code, old-style decomp — silenced like the rest).
    ${PETARI_ROOT}/src/JSystem/JMath/JMATrigonometric.cpp
    ${PETARI_ROOT}/src/Game/System/WPadButton.cpp
    ${PETARI_ROOT}/src/Game/System/WPadStick.cpp
    ${PETARI_ROOT}/src/Game/System/WPadAcceleration.cpp
    ${PETARI_ROOT}/src/Game/System/WPadRumble.cpp
    ${PETARI_ROOT}/src/Game/System/WPadRumbleData.cpp
    ${PETARI_ROOT}/src/Game/System/WPadInfoChecker.cpp
    ${PETARI_ROOT}/src/Game/System/WPadLeaveWatcher.cpp
)
set_source_files_properties(${PETARI_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/w>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")
