// compat/game/SceneCompat.cpp — M9.4: the NameObj/Scene execution infrastructure.
//
// The vendored Scene/NameObj headers are the interface; this file provides the
// host implementations of the pieces whose real .cpp files pull in trees that
// are not host-compiled yet (DrawBuffer/J3DDrawBuffer, the huge
// SceneObjHolder switch, layout engine, save data…). The *semantics* are the
// console's, exactly as the vendored callers expect:
//
//   - NameObjCategoryList / NameObjListExecutor / SceneNameObjListExecutor:
//     real category lists (movement/calcAnim/draw) with add/remove/execute,
//     the way MR::connectToScene registers and CategoryList::execute runs.
//   - SceneObjHolder + MR::createSceneObj: the real per-scene object array;
//     newEachObj creates the host versions of the scene objects the boot
//     path needs (CameraContext, NameObjGroup, NameObjExecuteHolder,
//     StopSceneController, SceneNameObjMovementController).
//   - NameObjExecuteHolder: the real connect-to-scene registration record.
//   - SceneFunction/CategoryList: the vendored execution order (the body of
//     Game/Scene/SceneExecutor.cpp) minus the renderer-only calls
//     (loadViewMtx / entryDrawBuffer — nothing is drawn on the host yet).
//   - The small scenes (GameScene, IntermissionScene, PlayTimerScene,
//     ScenarioSelectScene, ScenarioDataParser) are host classes with the
//     vendored header surfaces; their bodies are no-ops until M10.
//   - SimpleLayout/IsbnManager/LayoutManager: the layout pieces are stubbed
//     (the Logo fades through black; the strap graphic needs the nw4r layout
//     engine, M9.5+).
//
// TODO(PC_PORT, M9.5+): GameScene real, SceneObjHolder switch full, draw
// buffer entry, layout engine, save data.

#include "Game/Camera/CameraContext.hpp"
#include "Game/NameObj/NameObjCategoryList.hpp"
#include "Game/NameObj/NameObjExecuteHolder.hpp"
#include "Game/NameObj/NameObjGroup.hpp"
#include "Game/NameObj/NameObjHolder.hpp"
#include "Game/NameObj/NameObjListExecutor.hpp"
#include "Game/Scene/GameScene.hpp"
#include "Game/Scene/IntermissionScene.hpp"
#include "Game/Scene/PlayTimerScene.hpp"
#include "Game/Scene/ScenarioSelectScene.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneNameObjListExecutor.hpp"
#include "Game/Scene/SceneNameObjMovementController.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Scene/StopSceneController.hpp"
#include "Game/Screen/IsbnManager.hpp"
#include "Game/Screen/SimpleLayout.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/ScenarioDataParser.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRDvdRipper.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// =============================================================================
// NameObjHolder — real registration (the controller's per-scene holder).
// =============================================================================

namespace {

// Flat storage (the console uses chunked batches; a single chunk with real
// add/find/clear semantics is equivalent for the boot path).
constexpr int kHolderCapacity = 0x1400;

int sHolderCount = 0;

} // namespace

NameObjHolder::NameObjHolder(int capacity) {
    const int size = capacity > 0 ? capacity : kHolderCapacity;
    mObjArray1.init(size);
    for (int i = 0; i < size; ++i) {
        mObjArray1[i] = nullptr;
    }
}

void NameObjHolder::add(NameObj* pObj) {
    if (pObj == nullptr) {
        return;
    }
    if (sHolderCount >= mObjArray1.size()) {
        std::fprintf(stderr, "[scene] NameObjHolder overflow (%d)\n", mObjArray1.size());
        return;
    }
    mObjArray1[sHolderCount] = pObj;
    ++sHolderCount;
}

void NameObjHolder::suspendAllObj() {
}
void NameObjHolder::resumeAllObj() {
}
void NameObjHolder::syncWithFlags() {
}

void NameObjHolder::callMethodAllObj(NameObjMethod method) {
    for (int i = 0; i < sHolderCount; ++i) {
        NameObj* pObj = mObjArray1[i];
        if (pObj != nullptr) {
            (pObj->*method)();
        }
    }
}

void NameObjHolder::clearArray() {
    sHolderCount = 0;
    for (int i = 0; i < mObjArray1.size(); ++i) {
        mObjArray1[i] = nullptr;
    }
}

NameObj* NameObjHolder::find(const char* pName) {
    // The decomp headers expose no name accessor on NameObj; the console
    // variant is only used by debug tools. Host: never matches.
    (void)pName;
    return nullptr;
}

// =============================================================================
// NameObjCategoryList — real per-category lists (chunked like the console).
// =============================================================================

namespace {

constexpr int kCategoryCount = 0x80;   // generous: Movement/Draw enums are sparse
constexpr int kCategoryBatchSize = 64;
constexpr int kCategoryBatches = 4;    // 256 objects per category

CategoryListInitialTable sCategoryTable[kCategoryCount];

const CategoryListInitialTable* getCategoryTable() {
    for (int i = 0; i < kCategoryCount; ++i) {
        sCategoryTable[i].mIndex = i;
        sCategoryTable[i].mCount = kCategoryBatchSize;
    }
    return sCategoryTable;
}

} // namespace

NameObjCategoryList::CategoryInfo::CategoryInfo() {
    mNameObjArr.init(kCategoryBatches * kCategoryBatchSize);
    for (int z = 0; z < kCategoryBatches * kCategoryBatchSize; ++z) {
        mNameObjArr[z] = nullptr;
    }
    _C = nullptr;
    mCheck = 0;
}

NameObjCategoryList::CategoryInfo::~CategoryInfo() {
}

NameObjCategoryList::NameObjCategoryList(u32 count, const CategoryListInitialTable* pTable, NameObjMethod method, bool, const char*) {
    (void)pTable;
    const u32 num = count < kCategoryCount ? count : kCategoryCount;
    mCategoryInfo.init(static_cast< s32 >(num));
    mDelegator = new NameObjRealDelegator< NameObjMethod >(method);
    _C = 0;
    _D = 0;
}

NameObjCategoryList::NameObjCategoryList(u32 count, const CategoryListInitialTable* pTable, NameObjMethodConst method, bool, const char*) {
    (void)pTable;
    const u32 num = count < kCategoryCount ? count : kCategoryCount;
    mCategoryInfo.init(static_cast< s32 >(num));
    mDelegatorConst = new NameObjRealDelegator< NameObjMethodConst >(method);
    _C = 0;
    _D = 0;
}

NameObjCategoryList::~NameObjCategoryList() {
    delete mDelegator;
    delete mDelegatorConst;
}

void NameObjCategoryList::execute(int category) {
    if (category < 0 || category >= mCategoryInfo.size()) {
        return;
    }
    CategoryInfo& cat = mCategoryInfo[category];
    const u32 used = cat.mCheck;
    for (u32 k = 0; k < used; ++k) {
        NameObj* pObj = cat.mNameObjArr[k];
        if (pObj != nullptr) {
            (*mDelegator)(pObj);
        }
    }
}

void NameObjCategoryList::incrementCheck(NameObj*, int) {
}

void NameObjCategoryList::allocateBuffer() {
}

void NameObjCategoryList::add(NameObj* pObj, int category) {
    if (pObj == nullptr || category < 0 || category >= mCategoryInfo.size()) {
        return;
    }
    CategoryInfo& cat = mCategoryInfo[category];
    if (cat.mCheck >= kCategoryBatches * kCategoryBatchSize) {
        std::fprintf(stderr, "[scene] category list %d full\n", category);
        return;
    }
    cat.mNameObjArr[cat.mCheck] = pObj;
    ++cat.mCheck;
}

void NameObjCategoryList::remove(NameObj* pObj, int category) {
    if (pObj == nullptr || category < 0 || category >= mCategoryInfo.size()) {
        return;
    }
    CategoryInfo& cat = mCategoryInfo[category];
    for (u32 k = 0; k < cat.mCheck; ++k) {
        if (cat.mNameObjArr[k] == pObj) {
            const u32 last = cat.mCheck - 1;
            cat.mNameObjArr[k] = cat.mNameObjArr[last];
            cat.mNameObjArr[last] = nullptr;
            --cat.mCheck;
            return;
        }
    }
}

void NameObjCategoryList::registerExecuteBeforeFunction(const MR::FunctorBase&, int) {
}

void NameObjCategoryList::initTable(u32, const CategoryListInitialTable*) {
}

// =============================================================================
// NameObjListExecutor / SceneNameObjListExecutor.
// =============================================================================

NameObjListExecutor::NameObjListExecutor() : mBufferHolder(nullptr), mMovementList(nullptr), mCalcAnimList(nullptr), mDrawList(nullptr) {
}

NameObjListExecutor::~NameObjListExecutor() {
    delete mMovementList;
    delete mCalcAnimList;
    delete mDrawList;
    delete mBufferHolder;
}

void NameObjListExecutor::init() {
    initMovementList();
    initCalcAnimList();
    initCalcViewAndEntryList();
    initDrawList();
}

s32 NameObjListExecutor::registerDrawBuffer(LiveActor*, int) {
    return -1;
}

void NameObjListExecutor::allocateDrawBufferActorList() {
}

void NameObjListExecutor::registerPreDrawFunction(const MR::FunctorBase&, int) {
}

void NameObjListExecutor::findLightInfo(LiveActor*, int, int) const {
}

void NameObjListExecutor::incrementCheckMovement(NameObj* pObj, int cat) {
    if (mMovementList != nullptr) {
        mMovementList->incrementCheck(pObj, cat);
    }
}

void NameObjListExecutor::incrementCheckCalcAnim(NameObj* pObj, int cat) {
    if (mCalcAnimList != nullptr) {
        mCalcAnimList->incrementCheck(pObj, cat);
    }
}

void NameObjListExecutor::incrementCheckDraw(NameObj* pObj, int cat) {
    if (mDrawList != nullptr) {
        mDrawList->incrementCheck(pObj, cat);
    }
}

void NameObjListExecutor::addToMovement(NameObj* pObj, int cat) {
    if (mMovementList != nullptr) {
        mMovementList->add(pObj, cat);
    }
}

void NameObjListExecutor::addToCalcAnim(NameObj* pObj, int cat) {
    if (mCalcAnimList != nullptr) {
        mCalcAnimList->add(pObj, cat);
    }
}

void NameObjListExecutor::addToDrawBuffer(LiveActor*, int, int) {
}

void NameObjListExecutor::addToDraw(NameObj* pObj, int cat) {
    if (mDrawList != nullptr) {
        mDrawList->add(pObj, cat);
    }
}

void NameObjListExecutor::removeToMovement(NameObj* pObj, int cat) {
    if (mMovementList != nullptr) {
        mMovementList->remove(pObj, cat);
    }
}

void NameObjListExecutor::removeToCalcAnim(NameObj* pObj, int cat) {
    if (mCalcAnimList != nullptr) {
        mCalcAnimList->remove(pObj, cat);
    }
}

void NameObjListExecutor::removeToDrawBuffer(LiveActor*, int, int) {
}

void NameObjListExecutor::removeToDraw(NameObj* pObj, int cat) {
    if (mDrawList != nullptr) {
        mDrawList->remove(pObj, cat);
    }
}

void NameObjListExecutor::executeMovement(int cat) {
    if (mMovementList != nullptr) {
        mMovementList->execute(cat);
    }
}

void NameObjListExecutor::executeCalcAnim(int cat) {
    if (mCalcAnimList != nullptr) {
        mCalcAnimList->execute(cat);
    }
}

void NameObjListExecutor::entryDrawBuffer2D() {
}

void NameObjListExecutor::entryDrawBuffer3D() {
}

void NameObjListExecutor::entryDrawBufferMirror() {
}

void NameObjListExecutor::drawOpa(int) {
}

void NameObjListExecutor::drawXlu(int) {
}

void NameObjListExecutor::executeDraw(int cat) {
    if (mDrawList != nullptr) {
        mDrawList->execute(cat);
    }
}

void SceneNameObjListExecutor::initMovementList() {
    mMovementList = new NameObjCategoryList(kCategoryCount, getCategoryTable(), &NameObj::movement, false, "movement");
}

void SceneNameObjListExecutor::initCalcAnimList() {
    mCalcAnimList = new NameObjCategoryList(kCategoryCount, getCategoryTable(), &NameObj::calcAnim, false, "calcAnim");
}

void SceneNameObjListExecutor::initCalcViewAndEntryList() {
    // No per-object list: view/entry goes through the (stubbed) draw buffer.
}

void SceneNameObjListExecutor::initDrawList() {
    mDrawList = new NameObjCategoryList(kCategoryCount, getCategoryTable(), &NameObj::draw, false, "draw");
}

// =============================================================================
// SceneObjHolder + MR::createSceneObj / getSceneObjHolder / isExistSceneObj.
// =============================================================================

SceneObjHolder::SceneObjHolder() {
    std::memset(mObj, 0, sizeof(mObj));
}

NameObj* SceneObjHolder::create(int id) {
    if (id < 0 || id >= SceneObj_NumMax) {
        return nullptr;
    }
    if (mObj[id] == nullptr) {
        mObj[id] = newEachObj(id);
    }
    return mObj[id];
}

NameObj* SceneObjHolder::getObj(int id) const {
    if (id < 0 || id >= SceneObj_NumMax) {
        return nullptr;
    }
    return mObj[id];
}

bool SceneObjHolder::isExist(int id) const {
    return getObj(id) != nullptr;
}

NameObj* SceneObjHolder::newEachObj(int id) {
    switch (id) {
    case SceneObj_CameraContext:
        return new CameraContext();
    case SceneObj_NameObjGroup:
        return new NameObjGroup("SceneNameObjGroup", 0x100);
    case SceneObj_NameObjExecuteHolder:
        return new NameObjExecuteHolder(0x100);
    case SceneObj_StopSceneController:
        return new StopSceneController();
    case SceneObj_SceneNameObjMovementController:
        return new SceneNameObjMovementController();
    default:
        // Not needed by the Logo boot path; created lazily per milestone.
        std::fprintf(stderr, "[scene] SceneObj %d requested on the host — not implemented\n", id);
        return nullptr;
    }
}

namespace MR {

SceneObjHolder* getSceneObjHolder() {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return nullptr;
    }
    return pSystem->mSceneController->getSceneObjHolder();
}

NameObj* createSceneObj(int id) {
    SceneObjHolder* pHolder = getSceneObjHolder();
    if (pHolder == nullptr) {
        return nullptr;
    }
    return pHolder->create(id);
}

bool isExistSceneObj(int id) {
    SceneObjHolder* pHolder = getSceneObjHolder();
    return pHolder != nullptr && pHolder->isExist(id);
}

} // namespace MR

// =============================================================================
// NameObjExecuteHolder — the real connect-to-scene registration record.
// =============================================================================

NameObjExecuteInfo::NameObjExecuteInfo()
    : mExecutedObj(nullptr), _4(0), _5(0), mMovementType(-1), mCalcAnimType(-1), mDrawType(-1), mDrawBufferType(-1), _A(-1) {
}

void NameObjExecuteInfo::setConnectInfo(NameObj* pObj, int movementType, int calcAnimType, int drawBufferType, int drawType) {
    mExecutedObj = pObj;
    _4 = 2;
    _5 = 2;
    mMovementType = static_cast< s8 >(movementType);
    mCalcAnimType = static_cast< s8 >(calcAnimType);
    mDrawBufferType = static_cast< s8 >(drawBufferType);
    mDrawType = static_cast< s8 >(drawType);
}

void NameObjExecuteInfo::connectToScene() {
    NameObj* pObj = mExecutedObj;
    if (pObj == nullptr) {
        return;
    }
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    NameObjListExecutor* pExecutor = pSystem->mSceneController->getNameObjListExecutor();
    if (pExecutor == nullptr) {
        return;
    }
    if (mMovementType >= 0) {
        pExecutor->addToMovement(pObj, mMovementType);
    }
    if (mCalcAnimType >= 0) {
        pExecutor->addToCalcAnim(pObj, mCalcAnimType);
    }
    if (mDrawType >= 0) {
        pExecutor->addToDraw(pObj, mDrawType);
    }
}

void NameObjExecuteInfo::disconnectToScene() {
    NameObj* pObj = mExecutedObj;
    if (pObj == nullptr) {
        return;
    }
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    NameObjListExecutor* pExecutor = pSystem->mSceneController->getNameObjListExecutor();
    if (pExecutor == nullptr) {
        return;
    }
    if (mMovementType >= 0) {
        pExecutor->removeToMovement(pObj, mMovementType);
    }
    if (mCalcAnimType >= 0) {
        pExecutor->removeToCalcAnim(pObj, mCalcAnimType);
    }
    if (mDrawType >= 0) {
        pExecutor->removeToDraw(pObj, mDrawType);
    }
}

void NameObjExecuteInfo::initConnectting() {
}

void NameObjExecuteInfo::requestConnect(u8*) {
}

void NameObjExecuteInfo::requestDisconnect(u8*, bool) {
}

void NameObjExecuteInfo::executeRequirementConnectMovement() {
}
void NameObjExecuteInfo::executeRequirementDisconnectMovement() {
}
void NameObjExecuteInfo::executeRequirementConnectDraw() {
}
void NameObjExecuteInfo::executeRequirementDisconnectDraw() {
}
void NameObjExecuteInfo::executeRequirementDisconnectDrawDelay() {
}
void NameObjExecuteInfo::requestMovementOn(int) {
}
void NameObjExecuteInfo::requestMovementOff(int) {
}
void NameObjExecuteInfo::findLightInfo() const {
}
void NameObjExecuteInfo::connectToDraw() {
}
void NameObjExecuteInfo::disconnectToDraw() {
}

NameObjExecuteHolder::NameObjExecuteHolder(int maxSize)
    : NameObj("NameObjExecuteHolder"), mExecuteArray(nullptr), mExecuteArrayMaxSize(maxSize), mExecuteArraySize(0), _18(false), _19(false),
      _1A(false), _1B(false), _1C(false) {
    if (maxSize > 0) {
        mExecuteArray = new NameObjExecuteInfo[maxSize];
    }
}

void NameObjExecuteHolder::registerActor(NameObj* pObj, int movementType, int calcAnimType, int drawBufferType, int drawType) {
    if (mExecuteArray == nullptr || mExecuteArraySize >= mExecuteArrayMaxSize) {
        return;
    }
    NameObjExecuteInfo& info = mExecuteArray[mExecuteArraySize];
    info.setConnectInfo(pObj, movementType, calcAnimType, drawBufferType, drawType);
    ++mExecuteArraySize;
    // The registration IS the connect on the host (the real holder defers
    // list insertion to its update; nothing updates a scene mid-init here).
    info.connectToScene();
}

void NameObjExecuteHolder::initConnectting() {
}

void NameObjExecuteHolder::connectToScene(NameObj* pObj) {
    for (int i = 0; i < mExecuteArraySize; ++i) {
        if (mExecuteArray[i].mExecutedObj == pObj) {
            mExecuteArray[i].connectToScene();
            return;
        }
    }
}

void NameObjExecuteHolder::connectToDraw(NameObj* pObj) {
    for (int i = 0; i < mExecuteArraySize; ++i) {
        if (mExecuteArray[i].mExecutedObj == pObj) {
            mExecuteArray[i].connectToDraw();
            return;
        }
    }
}

void NameObjExecuteHolder::disconnectToScene(NameObj* pObj) {
    for (int i = 0; i < mExecuteArraySize; ++i) {
        if (mExecuteArray[i].mExecutedObj == pObj) {
            mExecuteArray[i].disconnectToScene();
            return;
        }
    }
}

void NameObjExecuteHolder::disconnectToDraw(NameObj*) {
}

bool NameObjExecuteHolder::isConnectToDraw(const NameObj* pObj) const {
    for (int i = 0; i < mExecuteArraySize; ++i) {
        if (mExecuteArray[i].mExecutedObj == pObj) {
            return mExecuteArray[i].mDrawType >= 0;
        }
    }
    return false;
}

void NameObjExecuteHolder::executeRequirementConnectMovement() {
}
void NameObjExecuteHolder::executeRequirementDisconnectMovement() {
}
void NameObjExecuteHolder::executeRequirementConnectDraw() {
}
void NameObjExecuteHolder::executeRequirementDisconnectDraw() {
}
void NameObjExecuteHolder::executeRequirementDisconnectDrawDelay() {
}
void NameObjExecuteHolder::requestMovementOn(int) {
}
void NameObjExecuteHolder::requestMovementOff(int) {
}

NameObjExecuteInfo* NameObjExecuteHolder::getConnectToSceneInfo(const NameObj* pObj) const {
    for (int i = 0; i < mExecuteArraySize; ++i) {
        if (mExecuteArray[i].mExecutedObj == pObj) {
            return const_cast< NameObjExecuteInfo* >(&mExecuteArray[i]);
        }
    }
    return nullptr;
}

namespace MR {

void registerNameObjToExecuteHolder(NameObj* pObj, int movementType, int calcAnimType, int drawBufferType, int drawType) {
    NameObjExecuteHolder* pHolder = static_cast< NameObjExecuteHolder* >(createSceneObj(SceneObj_NameObjExecuteHolder));
    if (pHolder != nullptr) {
        pHolder->registerActor(pObj, movementType, calcAnimType, drawBufferType, drawType);
    }
}

void initConnectting() {
}
void connectToSceneTemporarily(NameObj*) {
}
void disconnectToSceneTemporarily(NameObj*) {
}
void connectToDrawTemporarily(NameObj*) {
}
void disconnectToDrawTemporarily(NameObj*) {
}
bool isConnectToDrawTemporarily(const NameObj*) {
    return false;
}
void executeRequirementConnectMovement() {
}
void executeRequirementDisconnectMovement() {
}
void executeRequirementConnectDraw() {
}
void executeRequirementDisconnectDraw() {
}
void executeRequirementDisconnectDrawDelay() {
}
void requestMovementOnWithCategory(int) {
}
void requestMovementOffWithCategory(int) {
}

} // namespace MR

// =============================================================================
// The scene objects the boot path creates through SceneObjHolder::newEachObj.
// =============================================================================

CameraContext::CameraContext() : NameObj("CameraContext") {
}
CameraContext::~CameraContext() {
}
void CameraContext::initParams() {
}

NameObjGroup::NameObjGroup(const char* pName, int maxSize) : NameObj(pName), _C(0), mObjectCount(0), mObjects(nullptr) {
    if (maxSize > 0) {
        mObjects = new NameObj*[maxSize];
        _C = maxSize;
    }
}
NameObjGroup::~NameObjGroup() {
    delete[] mObjects;
}
void NameObjGroup::registerObj(NameObj* pObj) {
    if (mObjects != nullptr && mObjectCount < _C) {
        mObjects[mObjectCount] = pObj;
        ++mObjectCount;
    }
}
void NameObjGroup::pauseOffAll() const {
}
void NameObjGroup::initObjArray(int) {
}

StopSceneDelayRequest::StopSceneDelayRequest() : NameObj("StopSceneDelayRequest"), mFrame(0), mDelay(0) {
}
void StopSceneDelayRequest::movement() {
    if (mDelay < 0) {
        return;
    }
    if (mDelay == 0) {
        if (mFrame > 0) {
            --mFrame;
        }
        return;
    }
    --mDelay;
}

StopSceneController::StopSceneController() : NameObj("StopSceneController"), mDelayRequestArray(nullptr), mFrame(0) {
    mDelayRequestArray = new NameObjGroup("StopSceneDelayRequestArray", 0x20);
}
void StopSceneController::movement() {
    if (mFrame > 0) {
        --mFrame;
    }
    for (s32 i = 0; i < mDelayRequestArray->getObjectCount(); ++i) {
        mDelayRequestArray->mObjects[i]->movement();
    }
}
void StopSceneController::requestStopScene(s32 frame) {
    mFrame = frame;
}
void StopSceneController::requestStopSceneDelay(s32 delay, s32 frame) {
    StopSceneDelayRequest* pReq = new StopSceneDelayRequest();
    pReq->movement();
    pReq->mDelay = delay;
    pReq->mFrame = frame;
}
bool StopSceneController::isSceneStopped() const {
    return mFrame > 0;
}

StopSceneStateControl::StopSceneStateControl() : NerveExecutor("StopSceneStateControl"), _8(MR::MovementControlType_0), _C(nullptr) {
}
void StopSceneStateControl::requestStopSceneFor(MR::MovementControlType type, const NameObj* pObj) {
    _8 = type;
    _C = pObj;
}
void StopSceneStateControl::requestStopSceneOverwrite(const NameObj* pObj) {
    _C = pObj;
}
void StopSceneStateControl::requestPlaySceneFor(MR::MovementControlType type, const NameObj* pObj) {
    _8 = type;
    _C = pObj;
}
void StopSceneStateControl::executeStopCategories(MR::MovementControlType) {
}
void StopSceneStateControl::exeNone() {
}
void StopSceneStateControl::exeStopped() {
}

SceneNameObjMovementController::SceneNameObjMovementController()
    : NameObj("SceneNameObjMovementController"), _C(false), mStopSceneStateControl(nullptr) {
    mStopSceneStateControl = new StopSceneStateControl();
}
void SceneNameObjMovementController::movement() {
}
void SceneNameObjMovementController::requestStopSceneFor(MR::MovementControlType type, const NameObj* pObj) {
    mStopSceneStateControl->requestStopSceneFor(type, pObj);
}
void SceneNameObjMovementController::requestStopSceneOverwrite(const NameObj* pObj) {
    mStopSceneStateControl->requestStopSceneOverwrite(pObj);
}
void SceneNameObjMovementController::requestPlaySceneFor(MR::MovementControlType type, const NameObj* pObj) {
    mStopSceneStateControl->requestPlaySceneFor(type, pObj);
}
void SceneNameObjMovementController::notifyRequestNameObjMovementOnOff(bool on) {
    _C = on;
}

namespace MR {

SceneNameObjMovementController* getSceneNameObjMovementController() {
    return static_cast< SceneNameObjMovementController* >(getSceneObj< SceneNameObjMovementController >(SceneObj_SceneNameObjMovementController));
}

void notifyRequestNameObjMovementOnOff() {
    SceneNameObjMovementController* pCtrl = getSceneNameObjMovementController();
    if (pCtrl != nullptr) {
        pCtrl->movement();
    }
}

} // namespace MR

// =============================================================================
// The scenes.
// =============================================================================

GameScene::GameScene() : Scene("GameScene"), _14(0), mScenarioCamera(nullptr), mPauseCtrl(nullptr), mPauseSeq(nullptr), mStageClearSeq(nullptr),
                         mDraw3D(false), _29(0) {
}
GameScene::~GameScene() {
}
void GameScene::init() {
}
void GameScene::start() {
}
void GameScene::update() {
}
void GameScene::draw() const {
}
void GameScene::calcAnim() {
}

IntermissionScene::IntermissionScene() : Scene("IntermissionScene") {
    mState[0] = '\0';
    _54 = 0;
}
void IntermissionScene::update() {
    ++_54;
}
void IntermissionScene::draw() const {
}
void IntermissionScene::setCurrentSceneControllerState(const char* pState, ...) {
    va_list args;
    va_start(args, pState);
    vsnprintf(mState, sizeof(mState), pState, args);
    va_end(args);
    _54 = 0;
}

PlayTimerScene::PlayTimerScene() : Scene("PlayTimerScene"), mTimeLimitLayout(nullptr), mTimeUpLayout(nullptr), mTimeUpWaitFrame(0), _20(nullptr) {
}
void PlayTimerScene::init() {
}
void PlayTimerScene::start() {
}
void PlayTimerScene::update() {
}
void PlayTimerScene::draw() const {
}
bool PlayTimerScene::isActive() const {
    return false;
}
bool PlayTimerScene::isEndGlobalTimer() const {
    return false;
}
void PlayTimerScene::stop() {
}
void PlayTimerScene::startTimeUp() {
}
void PlayTimerScene::exeNormal() {
}
void PlayTimerScene::exeTimeUp() {
}
void PlayTimerScene::exeFadeoutAfterTimeUp() {
}

ScenarioSelectScene::ScenarioSelectScene() : Scene("ScenarioSelectScene"), _14(0), _15(0), _16(0), _17(0), mScenarioLayout(nullptr),
                                             mCinemaFrame(nullptr), _20(nullptr), _24(nullptr), _28(0), _29(0), _2A(0), _2B(0),
                                             mEffectSystem(nullptr), mCameraContext(nullptr) {
}
ScenarioSelectScene::~ScenarioSelectScene() {
}
void ScenarioSelectScene::init() {
}
void ScenarioSelectScene::start() {
}
void ScenarioSelectScene::update() {
}
void ScenarioSelectScene::draw() const {
}
void ScenarioSelectScene::calcAnim() {
}
void ScenarioSelectScene::calcViewAndEntry() {
}
void ScenarioSelectScene::startBackground() {
}
bool ScenarioSelectScene::isActive() const {
    return false;
}
bool ScenarioSelectScene::isExecForeground() const {
    return false;
}
bool ScenarioSelectScene::isScenarioSelecting() const {
    return false;
}
void ScenarioSelectScene::validateScenarioSelect() {
}
void ScenarioSelectScene::requestReset(bool) {
}
bool ScenarioSelectScene::isResetEnd() const {
    return true;
}
void ScenarioSelectScene::setupCameraMtx() const {
}
bool ScenarioSelectScene::trySetCurrentScenarioNo() const {
    return false;
}
void ScenarioSelectScene::suspend() {
}
void ScenarioSelectScene::tryStartScreenToFrame() {
}
void ScenarioSelectScene::exeDeactive() {
}
void ScenarioSelectScene::exeInvalidScenarioSelect() {
}
void ScenarioSelectScene::exeWaitStartScenarioSelect() {
}
void ScenarioSelectScene::exeStartScenarioSelect() {
}
void ScenarioSelectScene::exeWaitScenarioSelect() {
}
void ScenarioSelectScene::exeWaitResumeInitializeThread() {
}
void ScenarioSelectScene::exeWaitInitializeEnd() {
}
void ScenarioSelectScene::exeWaitDisappearLayout() {
}
void ScenarioSelectScene::exeWaitResumeInitializeThreadIfRequestedReset() {
}
void ScenarioSelectScene::exeWaitResumeInitializeThreadIfCanceledSelect() {
}

ScenarioDataParser::ScenarioDataParser(const char* pName) : NameObj(pName) {
}

const ScenarioData* ScenarioDataParser::getScenarioData(const char*) const {
    return nullptr;
}

const ScenarioData* ScenarioDataParser::getScenarioData(s32) const {
    return nullptr;
}

GalaxyStatusAccessor ScenarioDataParser::makeAccessor(const char*) const {
    return GalaxyStatusAccessor(nullptr);
}

GalaxyStatusAccessor::GalaxyStatusAccessor(const ScenarioData*) {
}

ScenarioDataIter::ScenarioDataIter(const ScenarioDataParser* pParser, int cur) : mParser(pParser), mCur(cur) {
}
bool ScenarioDataIter::isEnd() const {
    return true;
}
void ScenarioDataIter::goNext() {
    ++mCur;
}
GalaxyStatusAccessor ScenarioDataIter::makeAccessor() const {
    return GalaxyStatusAccessor(nullptr);
}

ScenarioData::ScenarioData(const char*) {
}
s32 ScenarioData::getScenarioNum() const {
    return 0;
}
s32 ScenarioData::getPowerStarNum() const {
    return 0;
}
bool ScenarioData::getValueString(const char*, s32, const char**) const {
    return false;
}
const char* ScenarioData::getZoneName(s32) const {
    return nullptr;
}
ScenarioDataIter ScenarioData::getScenarioDataIter(s32) const {
    return ScenarioDataIter(nullptr, 0);
}
bool ScenarioData::getValueU32(const char*, s32, u32*) const {
    return false;
}
bool ScenarioData::getValueBool(const char*, s32, bool*) const {
    return false;
}

// =============================================================================
// Layouts — host-minimal (the nw4r layout engine is not host-compiled yet).
// The Logo's fader is real; the strap graphic is omitted (documented in
// docs/boot.md M9.4).
// =============================================================================

SimpleLayout::SimpleLayout(const char* pName, const char* pLayoutName, u32 a3, int drawType) : LayoutActor(pName, true) {
    int type = drawType >= 0 ? drawType : static_cast< int >(MR::DrawType_Layout);
    MR::connectToScene(this, static_cast< int >(MR::MovementType_Layout), static_cast< int >(MR::CalcAnimType_Layout), -1, type);
    initLayoutManager(pLayoutName, a3);
}

SimpleEffectLayout::SimpleEffectLayout(const char* pName, const char* pLayoutName, u32 a3, int a4) : SimpleLayout(pName, pLayoutName, a3, a4) {
    initEffectKeeper(0, nullptr, nullptr);
}

IsbnManager::IsbnManager(MEMAllocator* pAllocator) : _0(false), mpAllocator(pAllocator), mpLayout(nullptr), mpResAccessor(nullptr) {
}

IsbnManager::~IsbnManager() {
}

IsbnManager* IsbnManager::create(void*, MEMAllocator*) {
    return nullptr; // Cn-only ISBN layout — not needed by the host boot
}

void IsbnManager::setNumber(const wchar_t*, const wchar_t*, const wchar_t*) {
}
bool IsbnManager::calc(bool) {
    return false;
}
void IsbnManager::draw() {
}
void IsbnManager::reset() {
}
void IsbnManager::setAdjustRate(f32, f32) {
}
void IsbnManager::calculateView() {
}

// =============================================================================
// SceneFunction / CategoryList — the vendored execution order (from
// Game/Scene/SceneExecutor.cpp) with the renderer-only calls removed.
// =============================================================================

void SceneFunction::movementStopSceneController() {
    MR::createSceneObj(SceneObj_StopSceneController);
    NameObj* pObj = MR::getSceneObj< StopSceneController >(SceneObj_StopSceneController);
    if (pObj != nullptr) {
        static_cast< StopSceneController* >(pObj)->movement();
    }
}

void SceneFunction::executeMovementList() {
    StopSceneController* pStop = static_cast< StopSceneController* >(MR::createSceneObj(SceneObj_StopSceneController));
    if (pStop != nullptr && pStop->isSceneStopped()) {
        return;
    }

    MR::getSceneNameObjMovementController()->movement();
    MR::executeRequirementDisconnectDrawDelay();
    CategoryList::execute(MR::MovementType_StopSceneDelayRequest);
    CategoryList::execute(MR::MovementType_Camera);
    CategoryList::execute(MR::MovementType_MirrorCamera);
    CategoryList::execute(MR::MovementType_ClippingDirector);
    MR::executeRequirementConnectMovement();
    MR::executeRequirementConnectDraw();
    MR::executeRequirementDisconnectMovement();
    MR::executeRequirementDisconnectDraw();
    CategoryList::execute(MR::MovementType_ScreenEffect);
    CategoryList::execute(MR::MovementType_SensorHitChecker);
    CategoryList::execute(MR::MovementType_MsgSharedGroup);
    CategoryList::execute(MR::MovementType_UNK_0x07);
    CategoryList::execute(MR::MovementType_UNK_0x14);
    CategoryList::execute(MR::MovementType_TalkDirector);
    CategoryList::execute(MR::MovementType_DemoDirector);
    CategoryList::execute(MR::MovementType_UNK_0x0C);
    CategoryList::execute(MR::MovementType_ClippedMapParts);
    CategoryList::execute(MR::MovementType_Planet);
    CategoryList::execute(MR::MovementType_CollisionMapObj);
    CategoryList::execute(MR::MovementType_CollisionEnemy);
    CategoryList::execute(MR::CalcAnimType_ClippedMapParts);
    CategoryList::execute(MR::CalcAnimType_Planet);
    CategoryList::execute(MR::CalcAnimType_CollisionMapObj);
    CategoryList::execute(MR::CalcAnimType_CollisionEnemy);
    CategoryList::execute(MR::MovementType_CollisionDirector);
    CategoryList::execute(MR::MovementType_Environment);
    CategoryList::execute(MR::MovementType_MapObj);
    CategoryList::execute(MR::MovementType_MapObjDecoration);
    CategoryList::execute(MR::MovementType_UNK_0x15);
    CategoryList::execute(MR::MovementType_NPC);
    CategoryList::execute(MR::MovementType_Ride);
    CategoryList::execute(MR::MovementType_Player);
    CategoryList::execute(MR::MovementType_PlayerDecoration);
    CategoryList::execute(MR::MovementType_Enemy);
    CategoryList::execute(MR::MovementType_EnemyDecoration);
    CategoryList::execute(MR::MovementType_Item);
    CategoryList::execute(MR::MovementType_PlayerMessenger);
    CategoryList::execute(MR::MovementType_AreaObj);
    CategoryList::execute(MR::MovementType_Layout);
    CategoryList::execute(MR::MovementType_LayoutDecoration);
    CategoryList::execute(MR::MovementType_MovieSubtitles);
    CategoryList::execute(MR::MovementType_WipeLayout);
    CategoryList::execute(MR::MovementType_Movie);
    CategoryList::execute(MR::MovementType_Sky);
    CategoryList::execute(MR::MovementType_ImageEffect);
    CategoryList::execute(MR::MovementType_AudEffectDirector);
    CategoryList::execute(MR::MovementType_AudBgmConductor);
    CategoryList::execute(MR::MovementType_AudCameraWatcher);
    CategoryList::execute(MR::MovementType_CameraCover);
    CategoryList::execute(MR::MovementType_SwitchWatcherHolder);
}

void SceneFunction::executeMovementListOnPlayingMovie() {
    MR::getSceneNameObjMovementController()->movement();
    MR::executeRequirementDisconnectDrawDelay();
    CategoryList::execute(MR::MovementType_StopSceneDelayRequest);
    CategoryList::execute(MR::MovementType_Layout);
    CategoryList::execute(MR::MovementType_LayoutDecoration);
    CategoryList::execute(MR::MovementType_MovieSubtitles);
    CategoryList::execute(MR::MovementType_WipeLayout);
    CategoryList::execute(MR::MovementType_Movie);
}

void SceneFunction::executeCalcAnimList() {
    CategoryList::execute(MR::CalcAnimType_Environment);
    CategoryList::execute(MR::CalcAnimType_MapObj);
    CategoryList::execute(MR::CalcAnimType_NPC);
    CategoryList::execute(MR::CalcAnimType_Ride);
    CategoryList::execute(MR::CalcAnimType_Enemy);
    CategoryList::execute(MR::CalcAnimType_Player);
    CategoryList::execute(MR::CalcAnimType_PlayerDecoration);
    CategoryList::execute(MR::CalcAnimType_MapObjDecoration);
    CategoryList::execute(MR::CalcAnimType_Item);
    CategoryList::execute(MR::CalcAnimType_Layout);
    CategoryList::execute(MR::CalcAnimType_LayoutDecoration);
    CategoryList::execute(MR::CalcAnimType_MirrorMapObj);
    CategoryList::execute(MR::MovementType_ShadowControllerHolder);
}

void SceneFunction::executeCalcViewAndEntryList2D() {
}

void SceneFunction::executeCalcViewAndEntryList() {
}

void SceneFunction::executeDrawBufferListNormalOpaBeforeVolumeShadow() {
}
void SceneFunction::executeDrawBufferListNormalOpaBeforeSilhouette() {
}
void SceneFunction::executeDrawBufferListNormalOpa() {
}
void SceneFunction::executeDrawBufferListNormalXlu() {
}
void SceneFunction::executeDrawListOpa() {
}
void SceneFunction::executeDrawListXlu() {
}
void SceneFunction::executeDrawSilhouetteAndFillShadow() {
}
void SceneFunction::executeDrawAlphaShadow() {
}
void SceneFunction::executeDrawAfterIndirect() {
}
void SceneFunction::executeDrawImageEffect() {
}
void SceneFunction::executeDrawList2DNormal() {
}
void SceneFunction::executeDrawList2DMovie() {
}

void SceneFunction::createHioBasicNode(Scene*) {
}

void SceneFunction::initForNameObj() {
    MR::createSceneObj(SceneObj_NameObjExecuteHolder);
    MR::createSceneObj(SceneObj_StopSceneController);
    MR::createSceneObj(SceneObj_SceneNameObjMovementController);
}

void SceneFunction::allocateDrawBufferActorList() {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    NameObjListExecutor* pExecutor = pSystem->mSceneController->getNameObjListExecutor();
    if (pExecutor != nullptr) {
        pExecutor->allocateDrawBufferActorList();
    }
    MR::initConnectting();
}

void SceneFunction::startStageFileLoad() {
}
void SceneFunction::waitDoneStageFileLoad() {
}
void SceneFunction::startActorFileLoadCommon() {
}
void SceneFunction::startActorFileLoadScenario() {
}
void SceneFunction::startActorPlacement() {
}
void SceneFunction::initAfterScenarioSelected() {
}
void SceneFunction::initEffectSystem(u32, u32) {
}
void SceneFunction::initForLiveActor() {
}

void CategoryList::execute(MR::MovementType type) {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    NameObjListExecutor* pExecutor = pSystem->mSceneController->getNameObjListExecutor();
    if (pExecutor != nullptr) {
        pExecutor->executeMovement(static_cast< int >(type));
    }
}

void CategoryList::execute(MR::CalcAnimType type) {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    NameObjListExecutor* pExecutor = pSystem->mSceneController->getNameObjListExecutor();
    if (pExecutor != nullptr) {
        pExecutor->executeCalcAnim(static_cast< int >(type));
    }
}

void CategoryList::execute(MR::DrawType type) {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return;
    }
    NameObjListExecutor* pExecutor = pSystem->mSceneController->getNameObjListExecutor();
    if (pExecutor != nullptr) {
        pExecutor->executeDraw(static_cast< int >(type));
    }
}

void CategoryList::entryDrawBuffer2D() {
}
void CategoryList::entryDrawBuffer3D() {
}
void CategoryList::entryDrawBufferMirror() {
}

void CategoryList::drawOpa(MR::DrawBufferType) {
}
void CategoryList::drawXlu(MR::DrawBufferType) {
}

void CategoryList::requestMovementOn(MR::MovementType) {
    MR::requestMovementOnWithCategory(0);
}
void CategoryList::requestMovementOff(MR::MovementType) {
    MR::requestMovementOffWithCategory(0);
}

// =============================================================================
// MR:: — the layout/draw/util pair the vendored LogoScene needs (the real
// .cpp files pull the layout engine / J3D draw buffer; those arrive with
// M9.5/M10).
// =============================================================================

namespace MR {

// Game/Util/ObjUtil.cpp replacements.
void connectToScene(NameObj* pObj, int movementType, int calcAnimType, int drawBufferType, int drawType) {
    registerNameObjToExecuteHolder(pObj, movementType, calcAnimType, drawBufferType, drawType);
}

void connectToSceneLayout(NameObj* pObj) {
    connectToScene(pObj, static_cast< int >(MovementType_Layout), static_cast< int >(CalcAnimType_Layout), -1,
                   static_cast< int >(DrawType_Layout));
}

void connectToSceneLayoutMovementCalcAnim(NameObj* pObj) {
    connectToScene(pObj, static_cast< int >(MovementType_Layout), static_cast< int >(CalcAnimType_Layout), -1, -1);
}

void connectToSceneWipeLayout(NameObj* pObj) {
    connectToScene(pObj, static_cast< int >(MovementType_WipeLayout), -1, -1, static_cast< int >(DrawType_WipeLayout));
}

// Game/Util/LayoutUtil.cpp replacements.
SimpleLayout* createSimpleLayout(const char* pName, const char* pLayoutName, u32 a3) {
    return new SimpleLayout(pName, pLayoutName, a3, -1);
}

bool isStep(const LayoutActor* pActor, s32 step) {
    return pActor != nullptr && pActor->getNerveStep() == step;
}

bool isFirstStep(const LayoutActor* pActor) {
    return pActor != nullptr && pActor->getNerveStep() == 0;
}

bool isGreaterStep(const LayoutActor* pActor, s32 step) {
    return pActor != nullptr && pActor->getNerveStep() > step;
}

bool isGreaterEqualStep(const LayoutActor* pActor, s32 step) {
    return pActor != nullptr && pActor->getNerveStep() >= step;
}

void startAnim(LayoutActor*, const char*, u32) {
}

void startAnimAtFirstStep(LayoutActor*, const char*, u32) {
}

void startAnimAndSetFrameAndStop(LayoutActor*, const char*, f32, u32) {
}

void setAnimFrameAndStop(LayoutActor*, f32, u32) {
}

void setAnimFrameAndStopAtEnd(LayoutActor*, u32) {
}

void setAnimRate(LayoutActor*, f32, u32) {
}

// Game/Util/DrawUtil.cpp replacements (nothing is drawn on the host yet; the
// GX state calls are cheap no-ops with the compat layer).
void drawInit() {
}

void drawInitFor2DModel() {
}

void setupDrawForNW4RLayout(f32, bool) {
}

void clearZBuffer() {
}

void fillScreenSetup(const GXColor&) {
}

void fillScreen(const GXColor&) {
}

void fillScreenArea(const TVec2s&, const TVec2s&) {
}

// Game/Util/GamePadUtil.cpp replacement (no pad state in the boot test).
bool testCorePadTriggerAnyWithoutHome(s32) {
    return false;
}

// Game/System/Language.cpp replacement.
const char* getCurrentRegionPrefix() {
    return "US"; // not "Cn": the ISBN/logo-censorship branch is skipped
}

// Game/Util/SystemUtil.cpp additions.
bool isScreen16Per9() {
    return false;
}

// Game/Util/FileUtil.cpp addition.
void* loadToMainRAM(const char*, u8*, JKRHeap*, JKRDvdRipper::EAllocDirection) {
    return nullptr; // Cn-only path; not reached with the US region prefix
}

} // namespace MR

