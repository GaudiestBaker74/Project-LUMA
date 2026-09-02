// =============================================================================
// PC_PORT PATCH of the vendored Game/NameObj/NameObj.cpp (see
// patches/README.md).
//
// Change: NameObj::NameObj() only registers with the NameObjRegister singleton
// when it has been initialized.
//
// On the Wii, GameSystem::init() creates the register before any NameObj, and
// the register outlives every scene. On the PC, tool code and unit tests
// (M9.4 scene_test) construct NameObjs without a boot — the vendored code
// dereferences the null singleton (get()->add(this)) and dies. Skipping the
// registration is correct there: with no register, the object was never going
// to be tracked by any holder anyway.
// =============================================================================
#include "Game/NameObj/NameObj.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Scene/SceneNameObjMovementController.hpp"
#include "Game/Util/SingletonHolder.hpp"

#define FLAG_MOVEMENT_OFF 1u
#define FLAG_SUSPEND 2u
#define FLAG_RESUME 4u

NameObj::NameObj(const char* pName) : mName(pName), mFlag(), mExecutorIdx(-1) {
    NameObjRegister* pRegister = SingletonHolder< NameObjRegister >::get();

    // PC_PORT: no register (no boot) — nothing to register into.
    if (pRegister != nullptr) {
        pRegister->add(this);
    }
}

NameObj::~NameObj() {
}

void NameObj::init(const JMapInfoIter& rIter) {
}

void NameObj::initAfterPlacement() {
}

void NameObj::movement() {
}

void NameObj::draw() const {
}

void NameObj::calcAnim() {
}

void NameObj::calcViewAndEntry() {
}

void NameObj::initWithoutIter() {
    init(JMapInfoIter());
}

void NameObj::setName(const char* pName) {
    mName = pName;
}

void NameObj::executeMovement() {
    if ((mFlag & FLAG_MOVEMENT_OFF) == FLAG_MOVEMENT_OFF) {
        return;
    }

    movement();
}

void NameObj::requestSuspend() {
    if ((getFlag() & FLAG_RESUME) == FLAG_RESUME) {
        mFlag &= ~FLAG_RESUME;
    }

    mFlag |= FLAG_SUSPEND;
}

void NameObj::requestResume() {
    if ((getFlag() & FLAG_SUSPEND) == FLAG_SUSPEND) {
        mFlag &= ~FLAG_SUSPEND;
    }

    mFlag |= FLAG_RESUME;
}

void NameObj::syncWithFlags() {
    if ((getFlag() & FLAG_SUSPEND) == FLAG_SUSPEND) {
        mFlag &= ~FLAG_SUSPEND;
        mFlag |= FLAG_MOVEMENT_OFF;
    }

    if ((getFlag() & FLAG_RESUME) == FLAG_RESUME) {
        mFlag &= ~FLAG_RESUME;
        mFlag &= ~FLAG_MOVEMENT_OFF;
    }
}

void NameObjFunction::requestMovementOn(NameObj* pObj) {
    pObj->requestResume();
    MR::notifyRequestNameObjMovementOnOff();
}

void NameObjFunction::requestMovementOff(NameObj* pObj) {
    pObj->requestSuspend();
    MR::notifyRequestNameObjMovementOnOff();
}
