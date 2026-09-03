#pragma once

// =============================================================================
// PC_PORT shadow of petari's libs/nw4r/include/nw4r/misc.h (M9.5.3).
//
// Upstream typedefs IntPtr/PtrDiff as (unsigned/signed) long. That is
// pointer-sized on the LP64 Linux host but only 32-bit on Windows x64 (MSVC
// is LLP64: long stays 32-bit). ut::LinkList's GetNodeFromPointer /
// GetPointerFromNode reinterpret_cast object pointers through IntPtr, so on
// MSVC every object living above the 4 GiB mark gets its address truncated:
// Pane::AppendChild then links through a wild node pointer and writes
// mNext/mPrev to a garbage address (access violation). This fired on the
// first real layout build (WiiRemoteStrapReplace.brlyt, pane 'PicBG' at
// 0x0000025C...) — synthetic tests escaped it only because their arenas
// happened to land below 4 GiB.
//
// Same defect class as the heapCommon.h UIntPtr shadow (M9.5.1).
// uintptr_t/intptr_t are pointer-sized on every supported host.
// =============================================================================

#include <cstdint>

namespace nw4r {
    typedef std::uintptr_t IntPtr;
    typedef std::intptr_t PtrDiff;

    static_assert(sizeof(IntPtr) == sizeof(void*), "nw4r::IntPtr must be pointer-sized");
    static_assert(sizeof(PtrDiff) == sizeof(void*), "nw4r::PtrDiff must be pointer-sized");
};  // namespace nw4r
