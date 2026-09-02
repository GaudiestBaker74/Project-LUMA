// =============================================================================
// PC_PORT PATCH of the vendored Game/NameObj/NameObjRegister.cpp (see
// patches/README.md).
//
// Change: NameObjRegister::add() tolerates a null current holder.
//
// On the Wii, every NameObj is created while a holder is current (the holder
// is shut/spread between game states). On the PC boot prologue (M9.2) the
// first NameObjs (HomeButtonLayout and friends) are created before any holder
// exists — registration is a no-op until the real holder machinery lands
// (M9.4, NameObjHolder).
//
// NOTE: NameObj::NameObj (see the patched NameObj.cpp) already guards the
// singleton call, so `this` is never null here — no `this == nullptr` test:
// GCC's -fdelete-null-pointer-checks (-O2) would strip it anyway.
// =============================================================================
#include "Game/NameObj/NameObjRegister.hpp"

void NameObjRegister::setCurrentHolder(NameObjHolder* pHolder) {
    mHolder = pHolder;
}

void NameObjRegister::add(NameObj* pObj) {
    // PC_PORT: without a current holder there is nowhere to register — the
    // object is simply not tracked (see header comment).
    if (mHolder == nullptr) {
        return;
    }

    mHolder->add(pObj);
}

NameObjRegister::NameObjRegister() : mHolder(0) {
}
