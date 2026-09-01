// =============================================================================
// Host definitions for the small game-level symbols the compiled Game/System
// WPad wrappers reference, whose owning translation units are not compiled on
// the PC build yet:
//
//   * WPad.cpp        — not compiled: its ctor news a WPadHVSwing and its
//                       update() drives it, but the decomp has no
//                       WPadHVSwing.cpp (header-only, methods unmatched).
//   * WPadHolder.cpp  — not compiled: pulls GameSystem.hpp (M9).
//   * Game/Util/HashUtil.cpp, Game/Util/GamePadUtil.cpp — not compiled.
//
// Every body below is a VERBATIM copy of the vendored implementation (the
// owning file is cited in the comment), except the single PC_PORT note at
// WPad::getInfoCallback (null-check for MR::getWPad). When the owning files
// compile, DELETE the matching definitions here (duplicates would not link).
//
// MR::getWPad is NOT provided for real: it needs the WPadHolder singleton
// (M9); it returns nullptr until then, and is only reachable from
// WPad::getInfoCallback, which the current no-holder PC path never exercises.
// =============================================================================

#include <cctype>

#include "Game/System/WPad.hpp"
#include "Game/System/WPadInfoChecker.hpp"  // WPadInfoChecker::successGetInfo
#include "Game/System/WPadHolder.hpp"  // WPadReadDataInfo + MR::getWPad declaration
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"  // MR::getWPadMaxCount declaration
#include <revolution/wpad.h>

// WPadHolder.cpp:24 — KPADStatus* WPadReadDataInfo::getKPadStatus(u32) const
KPADStatus* WPadReadDataInfo::getKPadStatus(u32 index) const {
    if (index >= mValidStatusCount) {
        return nullptr;
    }

    return &mStatusArray[index];
}

// WPadHolder.cpp:31 — u32 WPadReadDataInfo::getValidStatusCount() const
u32 WPadReadDataInfo::getValidStatusCount() const {
    return mValidStatusCount;
}

// WPad.cpp:220 — KPADStatus* WPad::getKPadStatus(u32 index) const
KPADStatus* WPad::getKPadStatus(u32 index) const {
    return mReadInfo->getKPadStatus(index);
}

// WPad.cpp:224 — s32 WPad::getValidStatusCount() const
s32 WPad::getValidStatusCount() const {
    return mReadInfo->getValidStatusCount();
}

// WPad.cpp:137 — static callback for WPADGetInfoAsync. PC_PORT: the original
// dereferences MR::getWPad(chan) unconditionally; keep the null-check because
// the PC port has no WPadHolder yet (MR::getWPad returns nullptr until M9).
void WPad::getInfoCallback(s32 chan, s32 result) {
    if (result != WPAD_ERR_NONE) {
        return;
    }

    WPad* pWPad = MR::getWPad(chan);
    if (pWPad == nullptr) {
        return;
    }
    pWPad->mInfoChecker->successGetInfo();
}

// Game/Util/HashUtil.cpp:188 — u32 MR::getHashCode(const char*)
namespace MR {
u32 getHashCode(const char* pStr) {
    u32 hash;

    for (hash = 0; *pStr != '\0'; pStr++) {
        hash = *pStr + hash * 31;
    }

    return hash;
}

// Game/Util/HashUtil.cpp:198 — lowercased variant. The original used
// Metrowerks' _current_locale.ctype internals; standard tolower is equivalent
// for ASCII, which is all the game hashes.
u32 getHashCodeLower(const char* pStr) {
    u32 hash;

    for (hash = 0; *pStr != '\0'; pStr++) {
        hash = static_cast<u8>(std::tolower(static_cast<unsigned char>(*pStr))) + hash * 31;
    }

    return hash;
}

// Game/Util/GamePadUtil.cpp:274 — SMG exposes two Wiimotes.
u32 getWPadMaxCount() {
    return 2;
}

// WPad.cpp:143 — device-type check used by the accel/stick wrappers.
bool isDeviceFreeStyle(const KPADStatus* pStatus) {
    return pStatus != nullptr && pStatus->wpad_err == WPAD_ERR_NONE && pStatus->dev_type == WPAD_DEV_FREESTYLE;
}

// Game/Util/GamePadUtil.cpp — WPadHolder::getWPad behind the singleton.
// TODO(PC_PORT): returns nullptr until the WPadHolder compiles (M9); only
// reachable from WPad::getInfoCallback, which the no-op holder path never
// exercises today.
WPad* getWPad(s32) {
    return nullptr;
}
}  // namespace MR
