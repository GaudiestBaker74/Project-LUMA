// compat/game/GameBoot.cpp — host-side stubs of the GameSystem boot tree (M9.2).
//
// The vendored Game/System/GameSystem.cpp is compiled for real (patched:
// main -> gameMain). Its boot dependencies that would pull in huge trees
// (fonts, J2D layouts, scene/sequence directors, the stationed-archive
// loader, the audio system wrapper, JUT console/exception) are provided here
// as documented host stubs, following the project convention in
// SystemCompat.cpp: same class names and signatures as the vendored headers,
// TODO(PC_PORT) on each.
//
// Each stub is traded for the real vendored .cpp as its milestone lands:
//   - M9.3 ✅: MainLoopFramework/JUTVideo/JUTXfb/JUTDirectPrint/J2D* vendored
//     (the real vsync-paced frame loop, see compat/CMakeLists.txt)
//   - M9.4: GameSystemSceneController/GameSequenceDirector/StationedArchiveLoader
//   - M9.5: GameSystemFontHolder/HomeButtonLayout/LayoutActor/JUT consoles
//   - M8.5: AudSystemWrapper (real JAS host glue)
//
// The MR:: functions declared in SystemUtil/SequenceUtil/FileUtil headers are
// also stubbed here (those .cpp files are not compiled yet because they pull
// JKRMemArchive/GameData/… trees; the real files replace these one by one).
#include "Game/NameObj/NameObj.hpp"
#include "Game/Screen/HomeButtonLayout.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/SystemWipeHolder.hpp"
#include "Game/System/AudSystemWrapper.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/FunctionAsyncExecutor.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemErrorWatcher.hpp"
#include "Game/System/GameSystemException.hpp"
#include "Game/System/GameSystemFontHolder.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/GameSystemStationedArchiveLoader.hpp"
#include "Game/System/HomeButtonStateNotifier.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/MathUtil.hpp"           // MR::initAcosTable decl
#include "Game/Util/StringUtil.hpp"         // MR::strcasecmp / isEqualString decls
#include "Game/Util/MemoryUtil.hpp"         // MR::copyMemory / zeroMemory decls
#include "Game/Util/EventUtil.hpp"          // MR::isPlayerLuigi
#include "Game/Util/JMapIdInfo.hpp"         // MR::getInitializeStartIdInfo
#include "Game/Util/FileUtil.hpp"           // MR::isFileExist / removeResourceAndFileHolder...
#include "Game/Util/SceneUtil.hpp"          // MR::isEqualSceneName / getInitializeStartIdInfo
#include "Game/Util/ScreenUtil.hpp"         // MR::isSystemWipeActive
// MR::resetWPad is declared in the vendored WPadHolder.hpp (WPadHolder::resetWPad);
#include "Game/Util/SequenceUtil.hpp"       // MR::requestChangeScene decl
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"         // MR::startFunctionAsyncExecute decls
#include "Game/NameObj/NameObjHolder.hpp"   // NameObjHolder::add (M9.4 tree)
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRUnitHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTXfb.hpp>
#include <nw4r/lyt/init.h>
#include <revolution.h>

#include "Game/LiveActor/Spine.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <chrono>

// =============================================================================
// JUTVideo/JUTXfb/MainLoopFramework are now the REAL vendored files (M9.3,
// see compat/CMakeLists.txt); the M9.2 host stubs for them were removed.
// =============================================================================

// JKRAram::create: the AR (audio RAM) manager. The real one (JKRAram.cpp +
// JKRAramPiece/Stream + JKRDecomp) lands with the audio milestone (M8.5);
// GameSystem::init only checks that the call returns.
JKRAram* JKRAram::create(u32, u32, s32, s32, s32) {
    return nullptr; // TODO(PC_PORT, M8.5)
}

// JKRUnitHeap::create: Petari's JKRUnitHeap.cpp never implements create()
// and leaves several JKRHeap pure virtuals open (the class is abstract in the
// decomp). FunctionAsyncExecutor only needs a working allocator for its
// per-job infos — back "the unit heap" with a real JKRSolidHeap: allocation
// goes through the object's actual vtable (JKRSolidHeap), which is all the
// executor ever does with it.
JKRUnitHeap* JKRUnitHeap::create(u32, u32 size, u32, JKRHeap* pParent, bool) {
    return reinterpret_cast< JKRUnitHeap* >(JKRSolidHeap::create(size, pParent, false));
}

// nw4r::lyt::LytInit: layout engine init (M9.5).
namespace nw4r {
namespace lyt {
void LytInit() {
    // TODO(PC_PORT, M9.5): nw4r layout environment init.
}
} // namespace lyt
} // namespace nw4r

// =============================================================================
// LayoutActor — base of the layout objects. M9.4: real nerve machine (Spine
// is vendored); the layout manager (nw4r lyt) still arrives with M9.5.
// =============================================================================

LayoutActor::LayoutActor(const char* pName, bool isChangeFrame)
    : NameObj(pName), mLayoutManager(nullptr), mSpine(nullptr), mEffectKeeper(nullptr),
      mPointingTarget(nullptr) {
    (void)isChangeFrame;
    // TODO(PC_PORT, M9.5): real layout management (J2D/nw4r lyt). The fader's
    // logic only needs the nerve machine + draw, which are real here.
}

void LayoutActor::initNerve(const Nerve* pNerve) {
    mSpine = new Spine(this, pNerve);
}

void LayoutActor::setNerve(const Nerve* pNerve) const {
    if (mSpine != nullptr) {
        mSpine->setNerve(pNerve);
    }
}

bool LayoutActor::isNerve(const Nerve* pNerve) const {
    return mSpine != nullptr && mSpine->isCurrentNerve(pNerve);
}

s32 LayoutActor::getNerveStep() const {
    return mSpine != nullptr ? mSpine->mStep : 0;
}

void LayoutActor::updateSpine() {
    if (mSpine != nullptr) {
        mSpine->update();
    }
}

void LayoutActor::movement() {
    updateSpine();
}

void LayoutActor::draw() const {
}

void LayoutActor::calcAnim() {
}

void LayoutActor::appear() {
    mFlag.mIsDead = false;
}

void LayoutActor::kill() {
    mFlag.mIsDead = true;
}

void LayoutActor::initLayoutManager(const char*, u32) {
    // TODO(PC_PORT, M9.5): nw4r layout resource loading. Until then the
    // layout manager stays null (the strap graphic is not drawn).
}

void LayoutActor::initLayoutManagerNoConvertFilename(const char*, u32) {
}

void LayoutActor::initLayoutManagerWithTextBoxBufferLength(const char*, u32, u32) {
}

void LayoutActor::createPaneMtxRef(const char*) {
}

MtxPtr LayoutActor::getPaneMtxRef(const char*) {
    return nullptr;
}

void LayoutActor::initEffectKeeper(int, const char*, const EffectSystem*) {
}

void LayoutActor::initPointingTarget(int) {
}

LayoutManager* LayoutActor::getLayoutManager() const {
    return mLayoutManager;
}

TVec2f LayoutActor::getTrans() const {
    return TVec2f(0.0f, 0.0f);
}

void LayoutActor::setTrans(const TVec2f&) {
}


// =============================================================================
// HomeButtonLayout
// =============================================================================

HomeButtonLayout::HomeButtonLayout()
    : LayoutActor("HOMEボタン", false), mMenuContext(nullptr), _24(false), _25(false) {
    // TODO(PC_PORT, M9.5): the nw4r layout for the home button; M9.4+ wires
    // tryCorePadTriggerHome.
}

void HomeButtonLayout::init(const JMapInfoIter&) {}
void HomeButtonLayout::movement() {}
void HomeButtonLayout::draw() const {}
void HomeButtonLayout::forceToDeactive() {}
bool HomeButtonLayout::isActive() const { return false; }
void HomeButtonLayout::updateController() {}
bool HomeButtonLayout::tryCorePadTriggerHome() { return false; }
void HomeButtonLayout::exeDeactive() {}
void HomeButtonLayout::exeActive() {}

// =============================================================================
// AudSystemWrapper (the game-side audio system — real glue is M8.5)
// =============================================================================

AudSystemWrapper::AudSystemWrapper(JKRSolidHeap* solid, JKRHeap* heap)
    : mAudSystem(nullptr), _4(solid), _8(heap), mSmrRes(nullptr), mJaiSeqRes(nullptr),
      mJaiCordRes(nullptr), mJaiMeRes(nullptr), mJaiRemixSeqRes(nullptr), mSpkHeap(nullptr),
      mSpkRes(nullptr), _28(false), _29(false), _2A(false) {
    // TODO(PC_PORT, M8.5): build the real game audio system (JAS host glue).
}

void AudSystemWrapper::requestResourceForInitialize() {}
void AudSystemWrapper::createAudioSystem() {}
void AudSystemWrapper::createSoundNameConverter() {}
void AudSystemWrapper::updateRhythm() {}
void AudSystemWrapper::movement() {}
void AudSystemWrapper::stopAllSound(u32) {}
bool AudSystemWrapper::isLoadDoneWaveDataAtSystemInit() const { return true; }
void AudSystemWrapper::loadStaticWaveData() {}
bool AudSystemWrapper::isLoadDoneStaticWaveData() const { return true; }
void AudSystemWrapper::loadStageWaveData(const char*, const char*, bool) {}
bool AudSystemWrapper::isLoadDoneStageWaveData() const { return true; }
void AudSystemWrapper::loadScenarioWaveData(const char*, const char*, s32) {}
bool AudSystemWrapper::isLoadDoneScenarioWaveData() const { return true; }
bool AudSystemWrapper::isPermitToReset() const { return true; }
void AudSystemWrapper::prepareReset() {}
void AudSystemWrapper::requestReset(bool) {}
bool AudSystemWrapper::isResetDone() { return false; }
void AudSystemWrapper::resumeReset() {}
void AudSystemWrapper::receiveResourceForInitialize() {}

// =============================================================================
// GameSystemObjHolder
// =============================================================================

GameSystemObjHolder::GameSystemObjHolder()
    : mObjHolder(nullptr), mParticleResHolder(nullptr), mRenderModeObj(nullptr), _C(nullptr),
      _10(nullptr), _14(nullptr), mCaptureScreenDirector(nullptr), mScreenPreserver(nullptr),
      mAudioSystem(nullptr), mWPadHolder(nullptr), mFunctionAsyncExecutor(nullptr),
      mMessageHolder(nullptr), mStarPointerDirector(nullptr), mLanguage(0) {
}

void GameSystemObjHolder::init() {
    // The async executor must exist: GameSystem::exeInitializeAudio starts
    // the audio creation on it (SystemUtil's MR:: helpers use this holder).
    mFunctionAsyncExecutor = new FunctionAsyncExecutor();
}

void GameSystemObjHolder::initAfterStationedResourceLoaded() {}
void GameSystemObjHolder::initMessageResource() {}

void GameSystemObjHolder::createAudioSystem() {
    // Callback of MR::startFunctionAsyncExecute (runs on a worker thread):
    // the real version builds AudSystem from the JaiSeq/JaiMe archives — the
    // whole thing is deferred to M8.5.
    mAudioSystem = new AudSystemWrapper(nullptr, nullptr);
    mAudioSystem->createAudioSystem();
}

void GameSystemObjHolder::update() {}
void GameSystemObjHolder::updateAudioSystem() {}
void GameSystemObjHolder::clearRequestFileInfo(bool) {}
void GameSystemObjHolder::drawStarPointer() {}
void GameSystemObjHolder::drawBeforeEndRender() {}
void GameSystemObjHolder::captureIfAllowForScreenPreserver() {}
GXRenderModeObj* GameSystemObjHolder::getRenderModeObj() const { return mRenderModeObj; }
void GameSystemObjHolder::initDvd() {}
void GameSystemObjHolder::initNAND() {}
void GameSystemObjHolder::initAudio() {}
void GameSystemObjHolder::initRenderMode() {}
void GameSystemObjHolder::initNameObj() {}
void GameSystemObjHolder::initFunctionAsyncExecutor() {}
void GameSystemObjHolder::initResourceHolderManager() {}
void GameSystemObjHolder::initGameController() {}
void GameSystemObjHolder::initWPad() {}
void GameSystemObjHolder::initStarPointerDirector() {}
void GameSystemObjHolder::initDisplay() {}

// =============================================================================
// GameSystemFontHolder (M9.5: nw4r ResFont from embedded data / font archive)
// =============================================================================

GameSystemFontHolder::GameSystemFontHolder()
    : _0(nullptr), _4(nullptr), mEmbeddedMessageFont(nullptr), mMessageFont(nullptr),
      mPictureFont(nullptr), mMenuFont(nullptr), mNumberFont(nullptr), mCinemaFont(nullptr) {
}

nw4r::ut::Font* GameSystemFontHolder::getMessageFont() const {
    // PC_PORT member is nw4r::ut::ResFont* (incomplete from this header); cast
    // via void* — the real font is null until M9.5 anyway.
    return static_cast< nw4r::ut::Font* >(static_cast< void* >(mMessageFont));
}

void GameSystemFontHolder::createFontFromEmbeddedData() {
    // TODO(PC_PORT, M9.5): ResFont over the embedded font image.
    mEmbeddedMessageFont = nullptr;
}

void GameSystemFontHolder::createFontFromFile() {
    // TODO(PC_PORT, M9.5): ResFont over the staged FontFile (JKRMemArchive).
    mMessageFont = nullptr;
}

// =============================================================================
// GameSequenceDirector (real: M9.4 — sequence functions + save data)
// =============================================================================

GameSequenceDirector::GameSequenceDirector()
    : mGameDataTemporaryInGalaxy(nullptr), mGameSequenceProgress(nullptr),
      mSaveDataHandleSequence(nullptr), mNWC24Messenger(nullptr) {
}

void GameSequenceDirector::initAfterResourceLoaded() {}

void GameSequenceDirector::update() {
    // M9.4 (host replaces GameSequenceProgress, which owns this in the real
    // game): when the scene controller is ready, start the scene and kick the
    // system-archive load (GameSystem -> LoadStationedArchive -> Normal), the
    // same two calls GameSequenceProgress::update performs.
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    GameSystemSceneController* pController = pSystem->mSceneController;
    if (pController->isReadyToStartScene()) {
        pController->startScene();
        GameSystemFunction::tryToLoadSystemArchive();
    }
}

void GameSequenceDirector::draw() const {}
bool GameSequenceDirector::isInitializedGameDataHolder() const { return false; }
GameDataHolder* GameSequenceDirector::getGameDataHolder() { return nullptr; }
void GameSequenceDirector::executeOnSaveSuccess() {}
void GameSequenceDirector::executeJustBeforeSave() {}

// =============================================================================
// GameSystemStationedArchiveLoader (real: M9.4 — staged archive loading)
// =============================================================================

GameSystemStationedArchiveLoader::GameSystemStationedArchiveLoader()
    : NerveExecutor("GameSystemStationedArchiveLoader"), mHeapHolder(nullptr), _C(false) {
    // TODO(PC_PORT, M9.4): loads the staged archives for real (system/player/
    // others over VFS + JKRMemArchive). Until then: "done".
}

void GameSystemStationedArchiveLoader::update() {}
bool GameSystemStationedArchiveLoader::isDone() const { return true; }
bool GameSystemStationedArchiveLoader::isPreparedReset() const { return false; }
void GameSystemStationedArchiveLoader::prepareReset() {}
void GameSystemStationedArchiveLoader::requestChangeArchivePlayer(bool) {}
void GameSystemStationedArchiveLoader::exeLoadAudio1stWaveData() {}
void GameSystemStationedArchiveLoader::exeLoadStationedArchivePlayer() {}
void GameSystemStationedArchiveLoader::exeLoadStationedArchiveOthers() {}
void GameSystemStationedArchiveLoader::exeInitializeGameData() {}
void GameSystemStationedArchiveLoader::exeEnd() {}
void GameSystemStationedArchiveLoader::exeSuspended() {}
void GameSystemStationedArchiveLoader::exeChangeArchivePlayer() {}
bool GameSystemStationedArchiveLoader::trySuspend() { return false; }
bool GameSystemStationedArchiveLoader::tryAsyncExecuteIfNotSuspend(const MR::FunctorBase&, const char*) { return false; }
void GameSystemStationedArchiveLoader::startToLoadStationedArchivePlayer(bool) {}
void GameSystemStationedArchiveLoader::startToLoadStationedArchiveOthers() {}
void GameSystemStationedArchiveLoader::createAndAddPlayerArchives(bool) {}
void GameSystemStationedArchiveLoader::createAndAddOtherArchives() {}

// =============================================================================
// GameSystemErrorWatcher (no warnings on the host; real window/BatteryLayout
// UI lands with the layouts, M9.5)
// =============================================================================

GameSystemErrorWatcher::GameSystemErrorWatcher()
    : NerveExecutor("ErrorWatcher"), mWindow(nullptr), mMessage(nullptr), mUnplaggedTexMap(nullptr),
      mBatteryLayout(nullptr), mDriveStatus(0), _1C(nullptr), mWiiRemoteStatus(0),
      mCounterIgnoreCheckFreeStyle(0), mCounterDecideDisconnect(0), mWiiRemoteBattery(0),
      mPermissionUpdateWiiRemoteStatus(false) {
}

void GameSystemErrorWatcher::initAfterResourceLoaded() {}
void GameSystemErrorWatcher::movement() {}
void GameSystemErrorWatcher::draw() const {}
bool GameSystemErrorWatcher::isWarning() const { return false; }
bool GameSystemErrorWatcher::setPermissionUpdateWiiRemoteStatus(bool permission) {
    bool permissionPrev = mPermissionUpdateWiiRemoteStatus;
    mPermissionUpdateWiiRemoteStatus = permission;
    return permissionPrev;
}

// =============================================================================
// GameSystemException (JUT console/exception machinery is M9.5 territory)
// =============================================================================

void* GameSystemException::sMapFileUsingBuffer = nullptr;

void GameSystemException::init() {
    // TODO(PC_PORT, M9.5): JUTDirectPrint/JUTAssertion/JUTConsole/JUTException
    // host stubs — the real exceptions console needs the font + consoles.
}

void GameSystemException::handleException(OSError, OSContext*, u32, u32) {
    // TODO(PC_PORT, M9.5): print + trap like the Wii firmware does.
}

// =============================================================================
// DrawSyncManager (the GX fifo draw-done tracking — stubbed until the real
// renderer exists; start() is the only call the boot path makes)
// =============================================================================

DrawSyncManager* DrawSyncManager::sInstance = nullptr;

DrawSyncManager* DrawSyncManager::start(u32, s32) {
    return nullptr; // TODO(PC_PORT, M10): real FIFO draw-done sync.
}

void DrawSyncManager::prepareReset() {}
void DrawSyncManager::resetIfAborted() {}
void DrawSyncManager::end() {}

// Referenced by the vendored MainLoopFramework::handleGXAbortAlarm (never
// reached on the host: the GX watchdog alarm is cancelled right after
// GXDrawDone, see the null guard in the patched MainLoopFramework).
void DrawSyncManager::clearFifo() {}

// =============================================================================
// GameSystemResetAndPowerProcess (reset/Wii-menu flow — M9.4+ when layouts
// and the OS-level reset survive)
// =============================================================================

GameSystemResetAndPowerProcess::GameSystemResetAndPowerProcess()
    : LayoutActor("リセット・電源", false), mResetTriggerChecker(nullptr), mFadeinoutControl(nullptr),
      mCommandBlock(), mResetOperation(ResetOperation_Restart), _5C(true),
      mIsValidPowerOff(false), _5E(false) {
}

void GameSystemResetAndPowerProcess::init(const JMapInfoIter&) {}
void GameSystemResetAndPowerProcess::draw() const {}
void GameSystemResetAndPowerProcess::control() {}
bool GameSystemResetAndPowerProcess::isActive() const { return false; }
void GameSystemResetAndPowerProcess::setResetOperationApplicationReset() { mResetOperation = ResetOperation_ApplicationReset; }
void GameSystemResetAndPowerProcess::setResetOperationReturnToMenu() { mResetOperation = ResetOperation_ReturnToMenu; }
void GameSystemResetAndPowerProcess::requestReset(bool) {}
void GameSystemResetAndPowerProcess::requestGoWiiMenu(bool) {}
void GameSystemResetAndPowerProcess::notifyCheckDiskResult(bool) {}
void GameSystemResetAndPowerProcess::exePolling() {}
void GameSystemResetAndPowerProcess::exeWaitResetPermitted() {}
void GameSystemResetAndPowerProcess::exePrepareReset() {}
void GameSystemResetAndPowerProcess::exeReset() {}
void GameSystemResetAndPowerProcess::exeWaitPrepareFadein() {}
void GameSystemResetAndPowerProcess::exeFadein() {}
void GameSystemResetAndPowerProcess::exitApplication() {
    // TODO(PC_PORT, M9.4): the host equivalent of OSReturnToMenu/OSRebootSystem.
}
bool GameSystemResetAndPowerProcess::tryPermitReset() { return true; }
bool GameSystemResetAndPowerProcess::tryAcceptPowerOff() { return false; }
bool GameSystemResetAndPowerProcess::isResetAcceptAudio() const { return false; }
void GameSystemResetAndPowerProcess::handleOSPowerCallback() {}
void GameSystemResetAndPowerProcess::handleCheckDiskAsync(s32, DVDCommandBlock*) {}

// =============================================================================
// HomeButtonStateNotifier (the real one needs MoviePlayerSimple — M9.5)
// =============================================================================

HomeButtonStateNotifier::HomeButtonStateNotifier() : _0(false), mMoviePlayer(nullptr) {}
void HomeButtonStateNotifier::update(bool param1) { _0 = param1; }
void HomeButtonStateNotifier::registerMoviePlayerSimple(MoviePlayerSimple*) {}
void HomeButtonStateNotifier::unregisterMoviePlayerSimple(MoviePlayerSimple*) {}
void HomeButtonStateNotifier::notifyHomeButtonDeactive() {}

// =============================================================================
// GameSystemFunction (header/Game/System/GameSystemFunction.hpp) — host
// semantics: the boot only reaches isResetProcessing/isOccurredSystemWarning/
// isActiveSaveDataHandleSequence; everything else is stubbed until its owner
// (SaveDataHandleSequence, controllers, NWC24…) ships.
// =============================================================================

namespace GameSystemFunction {

void loadAudioStaticWaveData() {}
bool isLoadedAudioStaticWaveData() { return true; }

void initAfterStationedResourceLoaded() {
    SingletonHolder< GameSystem >::get()->initAfterStationedResourceLoaded();
}

void setSceneNameObjHolderToNameObjRegister() {
    // TODO(PC_PORT, M9.4): NameObjRegister::setCurrentHolder(scene holder).
}

bool isCreatedGameDataHolder() { return false; }
bool isCreatedSystemWipe() { return SingletonHolder< GameSystem >::get()->mSystemWipeHolder != nullptr; }
bool isDoneLoadSystemArchive() { return SingletonHolder< GameSystem >::get()->isDoneLoadSystemArchive(); }

bool tryToLoadSystemArchive() {
    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    bool isExecute = pGameSystem->isExecuteLoadSystemArchive();
    if (!isExecute) {
        pGameSystem->startToLoadSystemArchive();
    }
    return !isExecute;
}

void requestChangeArchivePlayer(bool isPlayerMario) {
    SingletonHolder< GameSystem >::get()->mStationedArchiveLoader->requestChangeArchivePlayer(isPlayerMario);
}

bool isEndChangeArchivePlayer() { return SingletonHolder< GameSystem >::get()->mStationedArchiveLoader->isDone(); }

void activateScreenPreserver() {
    // TODO(PC_PORT, M9.4): ScreenPreserver::activate (null holder on the host).
}

void deactivateScreenPreserver() {
    // TODO(PC_PORT, M9.4): ScreenPreserver::deactivate.
}

bool isOccurredSystemWarning() { return SingletonHolder< GameSystem >::get()->mErrorWatcher->isWarning(); }
bool isResetProcessing() { return SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive(); }

void setResetOperationApplicationReset() {
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->setResetOperationApplicationReset();
}

void setResetOperationReturnToMenu() {
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->setResetOperationReturnToMenu();
}

void requestResetGameSystem(bool param1) { SingletonHolder< GameSystemResetAndPowerProcess >::get()->requestReset(param1); }
void requestGoWiiMenu(bool param1) { SingletonHolder< GameSystemResetAndPowerProcess >::get()->requestGoWiiMenu(param1); }

void forceToDeactivateHomeButtonLayout() {
    HomeButtonLayout* pLayout = SingletonHolder< GameSystem >::get()->mHomeButtonLayout;
    if (pLayout != nullptr) {
        pLayout->forceToDeactive();
    }
}

void resetCurrentScenarioNo() { SingletonHolder< GameSystem >::get()->mSceneController->resetCurrentScenarioNo(); }
bool isPermitToResetSaveDataHandleSequence() { return true; }
void prepareResetSaveDataHandleSequence() {}
bool isPrepareResetSaveDataHandleSequence() { return true; }
void restoreFromResetSaveDataHandleSequence() {}
bool isPermitToResetAudioSystem() { return true; }
void prepareResetAudioSystem() {}
void requestResetAudioSystem(bool) {}
bool isDoneResetAudioSystem() { return true; }
void resumeResetAudioSystem() {}
void stopControllerLeaveWatcher() {}
void startControllerLeaveWatcher() {}
void restartControllerLeaveWatcher() {}
void setPadConnectCallback() {}
void resetAllControllerRumble() {}
void setAutoSleepTimeWiiRemote(bool) {}
bool setPermissionToCheckWiiRemoteConnectAndScreenDimming(bool) { return true; }
void onPauseBeginAllRumble() {}
void onPauseEndAllRumble() {}
void onHomeButtonMenuBeginAllRumble() {}
void onHomeButtonMenuCloseAllRumble() {}
void onHomeButtonMenuEndAllRumble() {}
void prepareResetSystem() {}
bool isPreparedFadeinSystem() { return true; }
void restartSceneController() { SingletonHolder< GameSystem >::get()->mSceneController->restartGameAfterResetting(); }
bool isDisplayStrapRemineder() { return false; }

} // namespace GameSystemFunction

// =============================================================================
// GameSequenceFunction — the boot only touches isActiveSaveDataHandleSequence
// (GameSystem::update). The real file pulls the save-data/progress trees;
// it returns with M9.4/M11.
// =============================================================================

namespace GameSequenceFunction {

bool isActiveSaveDataHandleSequence() {
    // TODO(PC_PORT, M11): delegate to the real SaveDataHandleSequence.
    return false;
}

void startPreLoadSaveDataSequence() {
    // TODO(PC_PORT, M11): SaveDataHandleSequence::startPreLoad.
}

void notifyToGameSequenceProgressToEndScene() {
    // TODO(PC_PORT, M9.5): after the Logo, progress requests the next scene
    // (Title). Until then the Logo holds the boot in its Deactive nerve.
}

void requestChangeScene(const char* pName) {
    // Real semantics (GameSequenceFunction.cpp): stage the next scene info
    // and poke the controller's request state machine.
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    GameSystemSceneController* pController = pSystem->mSceneController;
    std::snprintf(pController->mNextSceneControlInfo.mScene, sizeof(pController->mNextSceneControlInfo.mScene), "%s", pName);
    pController->mNextSceneControlInfo.mScenarioNo = -1;
    pController->mNextSceneControlInfo.mSelectedScenarioNo = -1;
    pController->requestChangeScene();
}

} // namespace GameSequenceFunction

// =============================================================================
// MR:: — the small host replacements of Game/Util/SystemUtil.cpp,
// SequenceUtil.cpp and FileUtil.cpp (those pull JKRMemArchive / GameData /
// save-data trees; they return with M9.4/M9.5).
// =============================================================================

namespace MR {

// MemoryUtil.cpp instantiates JKRHeapAllocator<0> but the template's static
// data members are never defined (Petari relies on MWC's implicit inline
// statics); provide the N=0 instantiation host-side.
template<> MEMAllocator MR::JKRHeapAllocator< 0 >::sAllocator;
template<> MEMAllocatorFunc MR::JKRHeapAllocator< 0 >::sAllocatorFunc;
template<> JKRHeap* MR::JKRHeapAllocator< 0 >::sHeap = nullptr;

void initAcosTable() {
    // TODO(PC_PORT): the real MathUtil.cpp builds gAcosTable[256] on the
    // system heap; no host JMath table infra yet (M9.4+ math port).
}

void setLayoutDefaultAllocator() {
    // TODO(PC_PORT, M9.5): nw4r::lyt::Layout::mspAllocator = NewDeleteAllocator.
}

void startFunctionAsyncExecute(const MR::FunctorBase& rFunc, int priority, const char* pName) {
    FunctionAsyncExecutor* pExecutor = SingletonHolder< GameSystem >::get()->mObjHolder->mFunctionAsyncExecutor;
    pExecutor->start(rFunc, priority, pName);
}

void waitForEndFunctionAsyncExecute(const char* pName) {
    FunctionAsyncExecutor* pExecutor = SingletonHolder< GameSystem >::get()->mObjHolder->mFunctionAsyncExecutor;
    pExecutor->waitForEnd(pName);
}

bool isEndFunctionAsyncExecute(const char* pName) {
    FunctionAsyncExecutor* pExecutor = SingletonHolder< GameSystem >::get()->mObjHolder->mFunctionAsyncExecutor;
    return pExecutor->isEnd(pName);
}

f32 normalize(f32 val, f32 min, f32 max) {
    if (val <= min) {
        return 0.0f;
    }
    if (val >= max) {
        return 1.0f;
    }
    return (val - min) / (max - min);
}

f32 getLinerValue(f32 x, f32 start, f32 end, f32 max) {
    if (max <= 0.0f) {
        return end;
    }
    return start + (end - start) * normalize(x, 0.0f, max);
}

f32 getEaseInValue(f32 x, f32 start, f32 end, f32 max) {
    if (max <= 0.0f) {
        return end;
    }
    const f32 t = normalize(x, 0.0f, max);
    return start + (end - start) * t * t;
}

f32 getEaseOutValue(f32 x, f32 start, f32 end, f32 max) {
    if (max <= 0.0f) {
        return end;
    }
    const f32 t = normalize(x, 0.0f, max);
    const f32 u = 1.0f - t;
    return start + (end - start) * (1.0f - u * u);
}

f32 getEaseInOutValue(f32 x, f32 start, f32 end, f32 max) {
    if (max <= 0.0f) {
        return end;
    }
    const f32 t = normalize(x, 0.0f, max);
    if (t < 0.5f) {
        return start + (end - start) * 2.0f * t * t;
    }
    const f32 u = 1.0f - t;
    return start + (end - start) * (1.0f - 2.0f * u * u);
}

// Game/Util/SystemUtil.cpp replacement — the boot path only needs the
// async-executor trio below; requestChangeScene ("Logo") returns with the
// real scene controller in M9.4.
void requestChangeScene(const char* pName) {
    // SequenceUtil.cpp semantics: route through the scene controller.
    GameSequenceFunction::requestChangeScene(pName);
}

// FileUtil.cpp replacements (that file pulls JKRMemArchive — M9.4).
bool isFileExist(const char* pFilePath, bool) {
    return DVDConvertPathToEntrynum(pFilePath) >= 0;
}

u32 getFileSize(const char* pFilePath, bool) {
    // TODO(PC_PORT, M9.4): FST entry size once the real loader is in.
    return isFileExist(pFilePath, false) ? 0 : 0;
}

// MemoryUtil.cpp declares copyMemory/zeroMemory but the decomp has no bodies
// (MR::copyMemory / MR::zeroMemory are unresolved in upstream Petari).
void copyMemory(void* pDst, const void* pSrc, u32 size) {
    std::memcpy(pDst, pSrc, size);
}

void zeroMemory(void* pDst, u32 size) {
    std::memset(pDst, 0, size);
}

// SystemUtil.cpp additions (M9.4: the real scene controller's transition
// machinery). The boot path only needs presence + "no-op/empty" semantics.
bool tryEndFunctionAsyncExecute(const char* pName) {
    FunctionAsyncExecutor* pExecutor = SingletonHolder< GameSystem >::get()->mObjHolder->mFunctionAsyncExecutor;
    return pExecutor != nullptr && pExecutor->isEnd(pName);
}

void setRandomSeedFromStageName() {
}

void clearFileLoaderRequestFileInfo(bool) {
}

void stopAllSound(u32) {
}

// SceneUtil.cpp additions.
bool isEqualSceneName(const char* pName) {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return false;
    }
    return isEqualString(pSystem->mSceneController->mCurrSceneControlInfo.mScene, pName);
}

bool isEqualStageName(const char* pName) {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return false;
    }
    return isEqualString(pSystem->mSceneController->mCurrSceneControlInfo.mStage, pName);
}

const JMapIdInfo& getInitializeStartIdInfo() {
    static JMapIdInfo sStartIdInfo;
    return sStartIdInfo;
}

// EventUtil.cpp addition.
bool isPlayerLuigi() {
    return false;
}

// ScreenUtil.cpp addition.
bool isSystemWipeActive() {
    return false;
}

// FileUtil.cpp addition.
void removeResourceAndFileHolderIfIsEqualHeap(JKRHeap*) {
}

// WPadUtil.cpp addition.
void resetWPad() {
}

} // namespace MR

namespace MR {

// Game/Util/StringUtil.cpp replacements — the real file pulls the GameData
// tree (getGameMessageDirect, race times, isPlayerLuigi...), so only the two
// functions the M9.2 link set needs are provided host-side.
s32 strcasecmp(const char* pA, const char* pB) {
    for (;; ++pA, ++pB) {
        const unsigned char ca = static_cast< unsigned char >(std::tolower(static_cast< unsigned char >(*pA)));
        const unsigned char cb = static_cast< unsigned char >(std::tolower(static_cast< unsigned char >(*pB)));
        if (ca != cb) {
            return static_cast< s32 >(ca) - static_cast< s32 >(cb);
        }
        if (ca == '\0') {
            return 0;
        }
    }
}

bool isEqualString(const char* pA, const char* pB) {
    return std::strcmp(pA, pB) == 0;
}

} // namespace MR

// =============================================================================
// MR::createSystemWipeHolder (M9.4: real wipe holder for scene transitions)
// =============================================================================

namespace MR {

SystemWipeHolder* createSystemWipeHolder() {
    return nullptr; // TODO(PC_PORT, M9.4)
}

} // namespace MR
