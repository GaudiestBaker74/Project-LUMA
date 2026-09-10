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
//   async_execute_try_end_retires_job_and_ignores_stale_same_name (M9.5.4)
//                                      MR::tryEndFunctionAsyncExecute must
//                                      behave like the console SystemUtil: a
//                                      finished job is retired, and a NEW job
//                                      reusing the same name ("シーン初期化"
//                                      for every scene) is NOT reported done
//                                      because an older one finished. The
//                                      stale-entry bug made the scene
//                                      controller start the Title scene while
//                                      its init was still running on the
//                                      worker (TitleSequenceProduct::appear on
//                                      a null `this`).
// =============================================================================

#include "tests/test_runner.h"

#include "Game/Util/SingletonHolder.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Scene/LogoScene.hpp"
#include "Game/Scene/SceneFactory.hpp"
#include "Game/Screen/LogoFader.hpp"
#include "Game/System/FunctionAsyncExecutor.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/Functor.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>

#include <atomic>
#include <cstring>
#include <chrono>
#include <thread>

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

// ---------------------------------------------------------------------------
// M9.5.4 — regression for the Logo -> Title crash (see the header comment).
// Runs after game_system_boot_reaches_logo, which leaves the GameSystem (and
// its FunctionAsyncExecutor) alive for the process — exactly the state the
// real boot is in when the Title transition starts.
// ---------------------------------------------------------------------------
namespace async_probe {
    std::atomic< int > sFastRuns{0};
    std::atomic< int > sSlowRuns{0};
    std::atomic< bool > sSlowMayFinish{false};

    void fastJob() {
        sFastRuns++;
    }

    void slowJob() {
        // Hold the worker until the test lets it go: models a scene init
        // that is still running when the controller polls for completion.
        while (!sSlowMayFinish.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sSlowRuns++;
    }

    bool waitUntil(const std::atomic< int >& rCounter, int value, double seconds) {
        const auto tStart = std::chrono::steady_clock::now();
        while (rCounter.load() < value) {
            if (std::chrono::duration< double >(std::chrono::steady_clock::now() - tStart).count() > seconds) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    bool pollTryEnd(const char* pName, double seconds) {
        const auto tStart = std::chrono::steady_clock::now();
        while (!MR::tryEndFunctionAsyncExecute(pName)) {
            if (std::chrono::duration< double >(std::chrono::steady_clock::now() - tStart).count() > seconds) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
}  // namespace async_probe

TEST_CASE(async_execute_try_end_retires_job_and_ignores_stale_same_name) {
    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    if (pGameSystem == nullptr || pGameSystem->mObjHolder == nullptr ||
        pGameSystem->mObjHolder->mFunctionAsyncExecutor == nullptr) {
        SKIP("needs the GameSystem booted by game_system_boot_reaches_logo");
    }
    FunctionAsyncExecutor* pExecutor = pGameSystem->mObjHolder->mFunctionAsyncExecutor;

    // The same job name every scene transition uses on the console.
    static const char* const kJobName = "regression-scene-init";
    const int holdersBefore = pExecutor->mHolders.size();

    // 1) A job that finishes immediately: tryEnd must eventually report it
    //    done AND retire it (the console's waitForEnd erases the entry).
    async_probe::sFastRuns = 0;
    MR::startFunctionAsyncExecute(MR::Functor(&async_probe::fastJob), 17, kJobName);
    REQUIRE(async_probe::waitUntil(async_probe::sFastRuns, 1, 10.0));
    CHECK(async_probe::pollTryEnd(kJobName, 10.0));
    CHECK_EQ(pExecutor->mHolders.size(), holdersBefore);

    // 2) The next transition reuses the name while ITS job is still running.
    //    With the M9.4 host tryEnd (poll-only, never retire) the finished
    //    job from step 1 was still in the list and answered "done" for the
    //    new one -> the scene controller started the Title before its init
    //    finished. It must report NOT done until the slow job really ends.
    async_probe::sSlowRuns = 0;
    async_probe::sSlowMayFinish = false;
    MR::startFunctionAsyncExecute(MR::Functor(&async_probe::slowJob), 17, kJobName);

    // Give the worker time to pick the job up; poll like exeInitializeScene
    // does once per frame — every answer must be "not yet".
    bool reportedEarly = false;
    const auto tStart = std::chrono::steady_clock::now();
    while (std::chrono::duration< double >(std::chrono::steady_clock::now() - tStart).count() < 0.25) {
        if (MR::tryEndFunctionAsyncExecute(kJobName)) {
            reportedEarly = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(!reportedEarly);
    CHECK_EQ(async_probe::sSlowRuns.load(), 0);

    // 3) Release the worker: now it must complete and be retired.
    async_probe::sSlowMayFinish = true;
    REQUIRE(async_probe::waitUntil(async_probe::sSlowRuns, 1, 10.0));
    CHECK(async_probe::pollTryEnd(kJobName, 10.0));
    CHECK_EQ(pExecutor->mHolders.size(), holdersBefore);
}

} // namespace
