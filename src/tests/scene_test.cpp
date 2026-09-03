// =============================================================================
// M9.4: scene system + Logo.
//
//   scene_factory_creates_logo_scene   MR::createScene("Logo") really builds a
//                                      LogoScene (patched SceneFactory).
//   logo_fader_nerve_timeline          the vendored LogoFader's fade nerves
//                                      (real LayoutActor nerve machine).
//   game_system_boot_reaches_logo      the whole boot drive: GameSystem init
//                                      -> requestChangeScene("Logo") -> scene
//                                      controller machinery -> LogoScene
//                                      initialized -> its nerve chain runs to
//                                      Deactive (headless, no window).
// =============================================================================

#include "tests/test_runner.h"

#include "Game/Util/SingletonHolder.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Scene/LogoScene.hpp"
#include "Game/Scene/SceneFactory.hpp"
#include "Game/Screen/LogoFader.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>

#include <cstring>
#include <chrono>

namespace {

TEST_CASE(scene_factory_creates_logo_scene) {
    Scene* pScene = MR::createScene("Logo");
    REQUIRE(pScene != nullptr);
    CHECK(dynamic_cast< LogoScene* >(pScene) != nullptr);

    Scene* pGame = MR::createScene("Game");
    CHECK(pGame != nullptr);
    delete pGame;

    Scene* pNope = MR::createScene("NoSuchScene");
    CHECK(pNope == nullptr);

    delete pScene;
}

TEST_CASE(logo_fader_nerve_timeline) {
    LogoFader fader("test-fader");
    // M9.5.3a: LayoutActor::movement is the REAL vendored one now, which
    // early-outs while the actor is dead — and LogoFader's ctor kill()s it.
    // LogoScene does exactly this appear() before using the fader.
    fader.appear();
    fader.setBlank();
    CHECK(fader.isFadeEnd());

    // Fade-in: after mMaxStep (30) movements the fader reaches Display (rate 0).
    fader.startFadeIn();
    CHECK(!fader.isFadeEnd());
    for (int i = 0; i < 31; ++i) {
        fader.movement();
    }
    CHECK(fader.isFadeEnd());

    // Fade-out: back to Blank after the same number of steps.
    fader.startFadeOut();
    CHECK(!fader.isFadeEnd());
    for (int i = 0; i < 31; ++i) {
        fader.movement();
    }
    CHECK(fader.isFadeEnd());
}

TEST_CASE(game_system_boot_reaches_logo) {
    // --- the gameMain prologue (headless: VI compat + host heaps) ----------
    HeapMemoryWatcher::createRootHeap();
    SingletonHolder< HeapMemoryWatcher >::init();
    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();

    static const GXRenderModeObj sHostRenderMode = {
        VI_TVMODE_NTSC_PROG,
        640,
        456,
        456,
        (720 - 670) / 2,
        (480 - 456) / 2,
        670,
        456,
        VI_XFBMODE_SF,
        GX_FALSE,
        GX_FALSE,
        {
            {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6},
            {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6},
        },
        {32, 0, 32, 0, 0, 0, 0},
    };
    JUTVideo::createManager(&sHostRenderMode);
    JKRHeap* pHeap = JKRGetCurrentHeap();
    void* xfb1 = pHeap->alloc(0xA9600, 0x20);
    void* xfb2 = pHeap->alloc(0xA9600, 0x20);
    void* xfb3 = pHeap->alloc(0xA9600, 0x20);
    MainLoopFramework::sManager = MainLoopFramework::createManager(nullptr, xfb1, xfb2, xfb3, true);
    REQUIRE(MainLoopFramework::sManager != nullptr);

    MR::setLayoutDefaultAllocator();
    SingletonHolder< NameObjRegister >::init();
    SingletonHolder< GameSystem >::init();
    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    pGameSystem->init();

    // --- drive the boot: GameSystem nerves + scene controller --------------
    // InitializeAudio -> (async audio stub) -> InitializeLogoScene (requests
    // "Logo") -> scene controller: WaitDrawDone -> ChangeWaveBank ->
    // InitializeScene (async, creates LogoScene) -> ReadyToStartScene ->
    // startScene -> Normal -> LogoScene nerves: StrapFadein/Display/Fadeout ->
    // WaitReadDoneSystemArchive -> MountGameData -> Deactive.
    bool sawLogoScene = false;
    bool logoDeactivated = false;
    const auto tStart = std::chrono::steady_clock::now();
    int frame = 0;

    // The frame loop is free-running (no VI pacing): update() drives the
    // boot logic frame-by-frame while the parts the console does on worker
    // threads (async audio, async scene init) run in real time. The test is
    // bounded by wall-clock, not by a frame count.
    while (true) {
        pGameSystem->update();
        frame++;

        GameSystemSceneController* pController = pGameSystem->mSceneController;

        // The scene is created by an async init worker (exeInitializeScene ->
        // FunctionAsyncExecutor). Only touch the scene pointer once the
        // controller reports the init done: everything (incl. the LogoScene
        // nerve spine) is set up by then.
        if (pController->isSceneInitializeState(SceneInitializeState_End) && pController->mScene != nullptr) {
            LogoScene* pLogo = dynamic_cast< LogoScene* >(pController->mScene);
            if (pLogo != nullptr) {
                sawLogoScene = true;
                if (!pLogo->isDisplayStrapRemineder()) {
                    logoDeactivated = true;
                }
            }
        }

        // Drive until the logo deactivated AND the GameSystem moved on to
        // loading the system archive (host loader reports done immediately).
        if (logoDeactivated && pGameSystem->isDoneLoadSystemArchive()) {
            break;
        }

        if (std::chrono::duration< double >(std::chrono::steady_clock::now() - tStart).count() > 120.0) {
            break;
        }
    }

    CHECK(sawLogoScene);
    CHECK(logoDeactivated);

    // The GameSystem nerve chain must have reached Normal (system archive
    // "loaded" — host loader reports done immediately).
    CHECK(pGameSystem->isDoneLoadSystemArchive());
}

} // namespace
