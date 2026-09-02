// =============================================================================
// PC_PORT PATCH of the vendored Game/System/GameSystem.cpp (see
// patches/README.md).
//
// Change: `void main(void)` -> `void gameMain(void)`. On the Wii this is the
// DOL entry point; on PC the entry point is src/main.cpp, which calls
// gameMain() after Platform::init + compat::initOS. Nothing else changes.
// =============================================================================
#include "Game/System/GameSystem.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Screen/HomeButtonLayout.hpp"
#include "Game/Screen/SystemWipeHolder.hpp"
#include "Game/System/AudSystemWrapper.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/FileRipper.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystemDimmingWatcher.hpp"
#include "Game/System/GameSystemErrorWatcher.hpp"
#include "Game/System/GameSystemException.hpp"
#include "Game/System/GameSystemFontHolder.hpp"
#include "Game/System/GameSystemFrameControl.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/GameSystemStationedArchiveLoader.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/System/HomeButtonStateNotifier.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/Functor.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTDirectPrint.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <nw4r/lyt/init.h>
#include <revolution.h>

#define GX_FIFO_SIZE 0x80000

#define INIT_AUDIO_KEY "オーディオ初期化"  // "Audio Initialization"

namespace NrvGameSystem {
    NEW_NERVE(GameSystemInitializeAudio, GameSystem, InitializeAudio);
    NEW_NERVE(GameSystemInitializeLogoScene, GameSystem, InitializeLogoScene);
    NEW_NERVE(GameSystemLoadStationedArchive, GameSystem, LoadStationedArchive);
    NEW_NERVE(GameSystemWaitForReboot, GameSystem, WaitForReboot);
    NEW_NERVE(GameSystemNormal, GameSystem, Normal);
};  // namespace NrvGameSystem

void gameMain(void) {
    OSInitFastCast();
    DVDInit();
    VIInit();
    HeapMemoryWatcher::createRootHeap();
    OSInitMutex(&MR::MutexHolder< 0 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 1 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 2 >::sMutex);
    nw4r::lyt::LytInit();
    MR::setLayoutDefaultAllocator();
    SingletonHolder< HeapMemoryWatcher >::init();
    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    FileRipper::setup(0x20000, MR::getStationedHeapNapa());
    GameSystemException::init();
    MR::initAcosTable();
    // PC_PORT (M9.3): the console main equivalent of
    // GameSystemObjHolder::initRenderMode + initDisplay — the real VI manager
    // (vendored JUTVideo), the XFB triple buffer (vendored JUTXfb, created by
    // the MainLoopFramework ctor) and the vendored MainLoopFramework must
    // exist before GameSystem::init() news the GameSystemFrameControl. The
    // render mode is the host's: NTSC progressive 640x456 (the first
    // GXNtscProg entry's geometry, RenderMode.cpp); the 3 XFBs come from the
    // stationed heap like initDisplay's (0xA9600 =
    // MR::getRequiredExternalFrameBufferSize).
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
    JKRHeap* pStationedHeap = JKRGetCurrentHeap();
    void* xfb1 = pStationedHeap->alloc(0xA9600, 0x20);
    void* xfb2 = pStationedHeap->alloc(0xA9600, 0x20);
    void* xfb3 = pStationedHeap->alloc(0xA9600, 0x20);
    MainLoopFramework::sManager = MainLoopFramework::createManager(nullptr, xfb1, xfb2, xfb3, true);
    JUTDirectPrint::start();
    // PC_PORT (M9.2): the first NameObj (HomeButtonLayout / reset process)
    // registers through the NameObjRegister singleton; initialize it before
    // any NameObj exists (host stubs keep it holder-less, see the patched
    // NameObjRegister).
    SingletonHolder< NameObjRegister >::init();
    SingletonHolder< GameSystem >::init();
    SingletonHolder< GameSystem >::get()->init();

    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();

    while (true) {
        pGameSystem->frameLoop();
    }
}

GameSystem::GameSystem()
    : NerveExecutor("GameSystem"), mFifoBase(nullptr), mSequenceDirector(nullptr), mErrorWatcher(nullptr), mFontHolder(nullptr),
      mFrameControl(nullptr), mObjHolder(nullptr), mSceneController(nullptr), mStationedArchiveLoader(nullptr), mHomeButtonLayout(nullptr),
      mSystemWipeHolder(nullptr), mHomeButtonStateNotifier(nullptr), mIsExecuteLoadSystemArchive(false) {
}

void GameSystem::init() {
    JKRAram::create(0xE00000, 0xFFFFFFFF, 8, 7, 3);
    mObjHolder = new GameSystemObjHolder();
    mFontHolder = new GameSystemFontHolder();
    mFontHolder->createFontFromEmbeddedData();
    initNerve(&NrvGameSystem::GameSystemInitializeAudio::sInstance);
    mSequenceDirector = new GameSequenceDirector();
    initGX();
    DrawSyncManager::start(0x300, 15);
    mSceneController = new GameSystemSceneController();
    mObjHolder->init();
    mErrorWatcher = new GameSystemErrorWatcher();
    mFrameControl = new GameSystemFrameControl();
    SingletonHolder< GameSystemResetAndPowerProcess >::init();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->initWithoutIter();
    mStationedArchiveLoader = new GameSystemStationedArchiveLoader();
    mHomeButtonLayout = new HomeButtonLayout();
    mHomeButtonStateNotifier = new HomeButtonStateNotifier();
    mDimmingWatcher = new GameSystemDimmingWatcher();
    setNerve(&NrvGameSystem::GameSystemInitializeAudio::sInstance);
}

bool GameSystem::isExecuteLoadSystemArchive() const {
    return mIsExecuteLoadSystemArchive;
}

bool GameSystem::isDoneLoadSystemArchive() const {
    return isNerve(&NrvGameSystem::GameSystemNormal::sInstance);
}

void GameSystem::startToLoadSystemArchive() {
    mIsExecuteLoadSystemArchive = true;

    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    SingletonHolder< NameObjRegister >::get()->setCurrentHolder(mObjHolder->mObjHolder);
    setNerve(&NrvGameSystem::GameSystemLoadStationedArchive::sInstance);
}

void GameSystem::exeInitializeAudio() {
    if (MR::isFirstStep(this)) {
        MR::startFunctionAsyncExecute(MR::Functor_Inline(mObjHolder, &GameSystemObjHolder::createAudioSystem), 14, INIT_AUDIO_KEY);
    }

    updateSceneController();

    if (MR::isEndFunctionAsyncExecute(INIT_AUDIO_KEY) && mObjHolder->mAudioSystem->isLoadDoneWaveDataAtSystemInit()) {
        MR::waitForEndFunctionAsyncExecute(INIT_AUDIO_KEY);
        setNerve(&NrvGameSystem::GameSystemInitializeLogoScene::sInstance);
    }
}

void GameSystem::exeInitializeLogoScene() {
    if (GameSystemFunction::isResetProcessing()) {
        setNerve(&NrvGameSystem::GameSystemWaitForReboot::sInstance);
    } else {
        if (MR::isFirstStep(this)) {
            MR::requestChangeScene("Logo");
        }

        updateSceneController();
    }
}

void GameSystem::exeLoadStationedArchive() {
    mStationedArchiveLoader->update();
    updateSceneController();

    if (mStationedArchiveLoader->isDone()) {
        setNerve(&NrvGameSystem::GameSystemNormal::sInstance);
    }
}

void GameSystem::exeWaitForReboot() {
}

void GameSystem::exeNormal() {
    updateSceneController();
    mStationedArchiveLoader->update();
}

void GameSystem::initGX() {
    if (mFifoBase == nullptr) {
        mFifoBase = new (32) u8[GX_FIFO_SIZE];
    }

    GXInit(mFifoBase, GX_FIFO_SIZE);
}

void GameSystem::initAfterStationedResourceLoaded() {
    mFontHolder->createFontFromFile();
    mObjHolder->initAfterStationedResourceLoaded();
    mHomeButtonLayout->initWithoutIter();
    mErrorWatcher->initAfterResourceLoaded();
    mSystemWipeHolder = MR::createSystemWipeHolder();
    mSceneController->initAfterStationedResourceLoaded();
    mSequenceDirector->initAfterResourceLoaded();
}

void GameSystem::prepareReset() {
    mStationedArchiveLoader->prepareReset();
}

inline bool isSystemWaitForReboot(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(&NrvGameSystem::GameSystemWaitForReboot::sInstance);
}

inline bool isSystemNormal(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(&NrvGameSystem::GameSystemNormal::sInstance);
}

bool GameSystem::isPreparedReset() const {
    return isSystemWaitForReboot(this) || isSystemNormal(this) || mStationedArchiveLoader->isPreparedReset();
}

void GameSystem::frameLoop() {
    MainLoopFramework::sManager->beginRender();
    draw();
    MainLoopFramework::sManager->endRender();
    update();
    calcAnim();
    mObjHolder->captureIfAllowForScreenPreserver();
    MainLoopFramework::sManager->endFrame();
    MainLoopFramework::sManager->waitForRetrace();
}

void GameSystem::draw() {
    mSceneController->drawScene();
    mSequenceDirector->draw();
    mObjHolder->drawStarPointer();
    mObjHolder->drawBeforeEndRender();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->draw();
    }

    mErrorWatcher->draw();
    mHomeButtonLayout->draw();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->draw();
}

void GameSystem::update() {
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->movement();
    mSceneController->checkRequestAndChangeScene();
    mObjHolder->update();
    mHomeButtonLayout->movement();

    if (!mHomeButtonLayout->isActive()) {
        mErrorWatcher->movement();
    }

    mDimmingWatcher->_5 = mErrorWatcher->isWarning() || mHomeButtonLayout->isActive() || GameSequenceFunction::isActiveSaveDataHandleSequence();
    mDimmingWatcher->update();
    updateNerve();
}

void GameSystem::updateSceneController() {
    bool isSceneUpdate = true;
    bool isResetProcessing = SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive();

    mObjHolder->updateAudioSystem();

    if (isResetProcessing) {
        isSceneUpdate = false;
    }

    if (mHomeButtonLayout->isActive()) {
        isSceneUpdate = false;
    }

    if (GameSystemFunction::isOccurredSystemWarning()) {
        isSceneUpdate = false;
    }

    mHomeButtonStateNotifier->update(mHomeButtonLayout->isActive() || GameSystemFunction::isOccurredSystemWarning());

    if (isSceneUpdate || isResetProcessing) {
        mSequenceDirector->update();
    }

    if (isSceneUpdate || mSceneController->isFirstUpdateSceneNerveNormal()) {
        if (mSystemWipeHolder != nullptr) {
            mSystemWipeHolder->movement();
        }

        mSceneController->updateScene();
    }

    if (isResetProcessing) {
        mSceneController->updateSceneDuringResetProcessing();
    }
}

void GameSystem::calcAnim() {
    mSceneController->calcAnimScene();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->calcAnim();
    }
}
