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

# ---------------------------------------------------------------------------
# M9.5.2: nw4r — the Revolution "network/graphics" middleware the layouts and
# fonts are built on: lyt (brlyt layout engine), ut (brfnt fonts, text
# writers, containers), math (tri tables/types). Vendored clean unless a
# patches/nw4r override exists (host fixes go there, listed here instead).
# NOT compiled: lyt_arcResourceAccessor (the SMG game code reaches resources
# through LayoutHolder/JKRArchive; the RVL ARC subsystem it needs is not
# ported and its in-place BE parsing cannot work on the host anyway).
set(NW4R_SOURCES
    ${PETARI_ROOT}/src/nw4r/math/math_types.cpp
    ${PETARI_ROOT}/src/nw4r/math/math_triangular.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_CharStrmReader.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_CharWriter.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_Font.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_LinkList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/nw4r/ut/ut_ResFont.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_ResFontBase.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_RomFont.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_TagProcessorBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/nw4r/ut/ut_TextWriterBase.cpp
    ${PETARI_ROOT}/src/nw4r/ut/ut_binaryFileFormat.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_animation.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_bounding.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_common.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_drawInfo.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_group.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/nw4r/lyt/lyt_layout.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_material.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_pane.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_picture.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_resourceAccessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/nw4r/lyt/lyt_texMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/nw4r/lyt/lyt_textBox.cpp
    ${PETARI_ROOT}/src/nw4r/lyt/lyt_window.cpp
)

add_library(pc_compat STATIC
    os/OSArena.cpp
    os/OSMutex.cpp
    os/OSReport.cpp
    os/PPCIntrinsics.cpp  # __cntlzw etc. (PPC intrinsics used by vendored inline code)
    jsystem/JUTConsoleCompat.cpp
    jsystem/JUTExceptionCompat.cpp
    # M9.5.1: RARC (JKRArchive) host conversion — big-endian metadata swap +
    # on-disk 16-byte file entries -> host SDIFileEntry layout. Used by the
    # patched JKRMemArchive (see jsystem/RarcHost.cpp).
    jsystem/RarcHost.cpp
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
    # M9.5.2: the MEM allocator forwarders used by Layout::AllocMemory
    # (host implementation — the vendored mem_allocator.c drags in the RVL
    # expHeap header tree, and the host never uses MEM ExpHeaps: game code
    # allocates through JKRHeap-backed MEMAllocatorFuncs).
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/MEMAllocator.cpp
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
    # --- M8: audio pipeline (docs/audio.md) -----------------------------------
    # compat: jdsp microcode emulation (mixer + ADPCM), AI registers/DMA +
    # soft clock, 32-bit pointer token registry, JAS host glue (the audio
    # thread's run loop over Platform::Threading).
    audio/AudioCommon.cpp
    dsp/Adpcm.cpp
    dsp/DSP.cpp
    dsp/Mixer.cpp
    ai/AI.cpp
    jsystem/JasAudioHost.cpp
    # OS shims the driver needs (cache no-ops, interrupts, OSTime).
    os/OSCache.cpp
    os/OSInterrupt.cpp
    os/OSTime.cpp
    # Vendored JAudio2 driver (patched copies where the 32-bit pointer ABI or
    # the header overrides require it — see patches/README.md).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASAiCtrl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASCalc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASDSPInterface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASHeapCtrl.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASDriverIF.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASDSPChannel.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASLfo.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASProbe.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASCallback.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASCmdStack.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASReport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRSolidHeap.cpp
    # --- M9.1: OSThread/OSMessageQueue host layer + JKRThread -----------------
    os/OSThread.cpp
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRThread.cpp
    # --- M9.2: the gameMain boot prologue (docs/boot.md) -----------------------
    # Host stubs of the boot tree (font holder, audio wrapper, obj holder…) +
    # the patched vendored GameSystem/FileRipper/HeapMemoryWatcher + the real
    # vendored nerve/executor/util files.
    game/GameBoot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/GameSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/HeapMemoryWatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/FileRipper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/NameObj/NameObj.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/NameObj/NameObjRegister.cpp
    ${PETARI_ROOT}/src/Game/System/GameSystemFrameControl.cpp
    ${PETARI_ROOT}/src/Game/System/GameSystemDimmingWatcher.cpp
    ${PETARI_ROOT}/src/Game/System/FunctionAsyncExecutor.cpp
    ${PETARI_ROOT}/src/Game/System/OSThreadWrapper.cpp
    ${PETARI_ROOT}/src/Game/System/NerveExecutor.cpp
    ${PETARI_ROOT}/src/Game/LiveActor/Nerve.cpp
    ${PETARI_ROOT}/src/Game/LiveActor/Spine.cpp
    ${PETARI_ROOT}/src/Game/LiveActor/ActorStateKeeper.cpp
    ${PETARI_ROOT}/src/Game/Screen/LayoutActorFlag.cpp
    ${PETARI_ROOT}/src/Game/Util/NerveUtil.cpp
    ${PETARI_ROOT}/src/Game/Util/MemoryUtil.cpp
    ${PETARI_ROOT}/src/Game/Util/TriggerChecker.cpp
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRUnitHeap.cpp
    # --- M9.3: VI + frame control (docs/boot.md) -------------------------------
    os/OSAlarm.cpp
    vi/VICompat.cpp
    gx/GXSync.cpp
    jsystem/J2DGrafContextCompat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/MainLoopFramework.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Util/BothDirList.cpp
    ${PETARI_ROOT}/src/JSystem/JUtility/JUTVideo.cpp
    ${PETARI_ROOT}/src/JSystem/JUtility/JUTXfb.cpp
    ${PETARI_ROOT}/src/JSystem/JUtility/JUTDirectPrint.cpp
    ${PETARI_ROOT}/src/JSystem/J2DGraph/J2DOrthoGraph.cpp
    ${PETARI_ROOT}/src/JSystem/J2DGraph/J2DGrafContext.cpp
    ${PETARI_ROOT}/src/Game/Util/SchedulerUtil.cpp
    # --- M9.4: scene system + Logo (docs/boot.md) ------------------------------
    # Host implementations of the NameObj/Scene execution infrastructure +
    # the real vendored scene controller, Scene base and LogoFader, plus the
    # patched LogoScene/SceneFactory/StringUtil.
    game/SceneCompat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Scene/LogoScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Scene/SceneFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Util/StringUtil.cpp
    ${PETARI_ROOT}/src/Game/System/GameSystemSceneController.cpp
    ${PETARI_ROOT}/src/Game/Scene/Scene.cpp
    ${PETARI_ROOT}/src/Game/Screen/LogoFader.cpp
    # M9.3: patched mtx44 (no MWERKS asm) — plain C bodies, but the PC GX/mtx
    # header overrides are C++, so compile it as C++ like GXLight.c.
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/mtx/mtx44.c
    # --- M9.5.1: JKernel archive stack (RARC mount + Yaz0/Yay0) ---------------
    # The resource pipeline every layout/model/message load goes through:
    # MR::mountArchive/loadToMainRAM -> JKRArchive::mount -> JKRMemArchive ->
    # JKRDvdRipper/JKRDvdFile -> compat DVD; compressed payloads decode via
    # JKRDecomp. Vendored clean: the file-loader registry, the archive lookup
    # code, the archive iterators. Patched copies (see patches/README.md):
    # JKRArchivePub (MEM-only mount + missing statics), JKRMemArchive
    # (big-endian metadata + 20-byte on-disk file entries -> host layout, via
    # jsystem/RarcHost.cpp), JKRDvdFile (doneProcess back-pointer),
    # JKRDvdRipper (big-endian Yaz0 header, deterministic read errors),
    # JKRDecomp (synchronous decode, big-endian stream reads).
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRFileLoader.cpp
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRArchivePri.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRFileFinder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRArchivePub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRMemArchive.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRDvdFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRDvdRipper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRDecomp.cpp
    # M9.5.2: nw4r lyt/ut/math (vendored clean; see NW4R_SOURCES) + the host
    # resource converters (brlyt swapper, TPL native-tree builder).
    ${NW4R_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/nw4r/LytHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/nw4r/LytMissing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gx/TplHost.cpp
    # --- M9.5.3a: the real layout stack (load & build) ------------------------
    # Patched copies: LayoutHolder (RARC scan -> ResTables + the nw4r
    # ResourceAccessor), LayoutAnmPlayer (null-safe start + reconstructed
    # isStop), PaneEffectKeeper (effect-system stub), ResourceInfo (MSL_C
    # include). Vendored clean: LayoutActor, SimpleLayout, LayoutPaneCtrl.
    # Reconstructed: LayoutManager + the MR:: layout/anim/screen helpers
    # (LayoutManagerCompat.cpp) and J3DFrameCtrl extracted verbatim from the
    # huge J3DAnimation.cpp (the rest of J3D lands with M9.5.4).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/ResourceInfo.cpp
    ${PETARI_ROOT}/src/Game/Screen/LayoutActor.cpp
    ${PETARI_ROOT}/src/Game/Screen/SimpleLayout.cpp
    ${PETARI_ROOT}/src/Game/Screen/LayoutPaneCtrl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/LayoutHolder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Animation/LayoutAnmPlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Screen/PaneEffectKeeper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/game/LayoutManagerCompat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/jsystem/J3DFrameCtrlCompat.cpp
)
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/gx/GXLight.c
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/mtx/mtx44.c
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
    # nw4r headers (lyt layout engine, ut, ...) — used by GameSystem (LytInit)
    # and the layout/screen code.
    ${PETARI_ROOT}/libs/nw4r/include
    # Game headers ("Game/...") — needed by the M6.5 WPad wrapper integration
    # (and, later, all of Game/System).
    ${PETARI_ROOT}/include
)

target_link_libraries(pc_compat PUBLIC pc_platform)
if(WIN32)
    # timeBeginPeriod (VICompat's field-clock cadence) lives in winmm. MSVC
    # also picks it up from the #pragma comment(lib) in the source; linking it
    # here keeps MinGW/Clang-cl builds working too.
    target_link_libraries(pc_compat PUBLIC winmm)
endif()

# The vendored Petari sources are old-style decompiled code: silence warnings
# for them (see cmake/CompilerWarnings.cmake). Flag differs per toolchain:
# GCC/Clang use -w, MSVC uses /w.
set(PETARI_SOURCES
    ${JKR_EXPHEAP_SOURCE}
    ${JKR_HEAP_SOURCE}
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRDisposer.cpp
    ${PETARI_ROOT}/src/JSystem/JSupport/JSUList.cpp
    # M9.5.2: the MEM allocator forwarders used by Layout::AllocMemory
    # (host implementation — the vendored mem_allocator.c drags in the RVL
    # expHeap header tree, and the host never uses MEM ExpHeaps: game code
    # allocates through JKRHeap-backed MEMAllocatorFuncs).
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/MEMAllocator.cpp
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
    # M8 (audio): patched + vendored JAudio2 driver files.
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASAiCtrl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASCalc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASDSPInterface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JAudio2/JASHeapCtrl.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASDriverIF.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASDSPChannel.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASLfo.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASProbe.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASCallback.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASCmdStack.cpp
    ${PETARI_ROOT}/src/JSystem/JAudio2/JASReport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRSolidHeap.cpp
    # M9.1–M9.4: the boot tree, frame control, scene system (vendored +
    # patched copies — all old-style decomp, silenced like the rest).
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/GameSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/HeapMemoryWatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/FileRipper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/NameObj/NameObj.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/NameObj/NameObjRegister.cpp
    ${PETARI_ROOT}/src/Game/System/GameSystemFrameControl.cpp
    ${PETARI_ROOT}/src/Game/System/GameSystemDimmingWatcher.cpp
    ${PETARI_ROOT}/src/Game/System/FunctionAsyncExecutor.cpp
    ${PETARI_ROOT}/src/Game/System/OSThreadWrapper.cpp
    ${PETARI_ROOT}/src/Game/System/NerveExecutor.cpp
    ${PETARI_ROOT}/src/Game/LiveActor/Nerve.cpp
    ${PETARI_ROOT}/src/Game/LiveActor/Spine.cpp
    ${PETARI_ROOT}/src/Game/LiveActor/ActorStateKeeper.cpp
    ${PETARI_ROOT}/src/Game/Screen/LayoutActorFlag.cpp
    ${PETARI_ROOT}/src/Game/Util/NerveUtil.cpp
    ${PETARI_ROOT}/src/Game/Util/MemoryUtil.cpp
    ${PETARI_ROOT}/src/Game/Util/TriggerChecker.cpp
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRUnitHeap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/MainLoopFramework.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Util/BothDirList.cpp
    ${PETARI_ROOT}/src/JSystem/JUtility/JUTVideo.cpp
    ${PETARI_ROOT}/src/JSystem/JUtility/JUTXfb.cpp
    ${PETARI_ROOT}/src/JSystem/JUtility/JUTDirectPrint.cpp
    ${PETARI_ROOT}/src/JSystem/J2DGraph/J2DOrthoGraph.cpp
    ${PETARI_ROOT}/src/JSystem/J2DGraph/J2DGrafContext.cpp
    ${PETARI_ROOT}/src/Game/Util/SchedulerUtil.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Scene/LogoScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Scene/SceneFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Util/StringUtil.cpp
    ${PETARI_ROOT}/src/Game/System/GameSystemSceneController.cpp
    ${PETARI_ROOT}/src/Game/Scene/Scene.cpp
    ${PETARI_ROOT}/src/Game/Screen/LogoFader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/RVL_SDK/mtx/mtx44.c
    # M9.5.1: the JKernel archive stack — RARC mounting + Yaz0/Yay0 decode,
    # the resource pipeline every layout/model/message load goes through.
    # Vendored clean: the file-loader registry, the archive lookup code and
    # the archive iterators. Patched copies (see patches/README.md): the
    # public API (MEM-only mount), JKRMemArchive (big-endian metadata +
    # 20-byte on-disk file entries -> host layout, via jsystem/RarcHost.cpp),
    # JKRDvdFile (doneProcess back-pointer), JKRDvdRipper (big-endian Yaz0
    # header, deterministic read errors) and JKRDecomp (synchronous decode,
    # big-endian stream reads).
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRFileLoader.cpp
    ${PETARI_ROOT}/src/JSystem/JKernel/JKRArchivePri.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRFileFinder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRArchivePub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRMemArchive.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRDvdFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRDvdRipper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/JSystem/JKernel/JKRDecomp.cpp
    # M9.5.2: nw4r lyt/ut/math (vendored clean; see NW4R_SOURCES) + the host
    # resource converters (brlyt swapper, TPL native-tree builder).
    ${NW4R_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/nw4r/LytHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/nw4r/LytMissing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gx/TplHost.cpp
    # --- M9.5.3a: the real layout stack (load & build) ------------------------
    # Patched copies: LayoutHolder (RARC scan -> ResTables + the nw4r
    # ResourceAccessor), LayoutAnmPlayer (null-safe start + reconstructed
    # isStop), PaneEffectKeeper (effect-system stub), ResourceInfo (MSL_C
    # include). Vendored clean: LayoutActor, SimpleLayout, LayoutPaneCtrl.
    # Reconstructed: LayoutManager + the MR:: layout/anim/screen helpers
    # (LayoutManagerCompat.cpp) and J3DFrameCtrl extracted verbatim from the
    # huge J3DAnimation.cpp (the rest of J3D lands with M9.5.4).
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/ResourceInfo.cpp
    ${PETARI_ROOT}/src/Game/Screen/LayoutActor.cpp
    ${PETARI_ROOT}/src/Game/Screen/SimpleLayout.cpp
    ${PETARI_ROOT}/src/Game/Screen/LayoutPaneCtrl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/System/LayoutHolder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Animation/LayoutAnmPlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/Game/Screen/PaneEffectKeeper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/game/LayoutManagerCompat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/jsystem/J3DFrameCtrlCompat.cpp
)
set_source_files_properties(${PETARI_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/w>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")
