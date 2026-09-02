// =============================================================================
// Host implementations of the PPC intrinsics declared in
// compat/include/revolution/types.h (for non-Metrowerks toolchains).
//
// Vendored inline code uses these (e.g. __GDLightID2Index in
// revolution/gd/GDLight.h calls __cntlzw). Only the intrinsics actually
// referenced by compiled code are defined here; add more when a link error
// demands it. C++ linkage — matches how the vendored inline functions call
// them (they are not wrapped in extern "C" in the TUs compiled so far).
// =============================================================================

#include <revolution/types.h>

#include <cmath>
#include <cstring>

u32 __cntlzw(u32 value) {
    if (value == 0) {
        return 32;
    }
    return static_cast<u32>(__builtin_clz(value));
}

f32 __frsqrte(f32 value) {
    // PPC frsqrte = reciprocal square root ESTIMATE. Used by JGeometry's
    // TUtil::sqrt/inv_sqrt, which follow it with Newton-Raphson refinement
    // (so the estimate precision does not matter). For non-positive input
    // return the input unchanged, mirroring the PPC behaviour the inlines
    // rely on (TUtil guards val <= 0 before calling anyway).
    if (value <= 0.0f) {
        return value;
    }
    return 1.0f / std::sqrt(value);
}

f32 __fabsf(f32 value) {
    // PPC fabs instruction.
    return std::fabs(value);
}

s32 __abs(s32 value) {
    // PPC abs (integer absolute value).
    return value < 0 ? -value : value;
}

void* __memcpy(void* dst, const void* src, int size) {
    // PPC memcpy intrinsic (dolphin's __memcpy has the same signature).
    return std::memcpy(dst, src, static_cast<std::size_t>(size));
}

// --- Metrowerks runtime conversion helpers (compat/include/runtime.h) -------
// The DOL runtime provided these in asm; the decompiled code calls them by
// name (e.g. MainLoopFramework::waitDrawDoneAndSetAlarm converts the GX
// watchdog interval with __cvt_dbl_usll). C linkage — matches the extern "C"
// declarations in the runtime.h shim. Semantics: truncation toward zero with
// saturation (NaN -> 0), the behaviour MSL documents for the out-of-range
// cases the hardware leaves undefined.
#include <runtime.h>

extern "C" unsigned long long __cvt_dbl_usll(double value) {
    if (value != value) { // NaN
        return 0;
    }
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 18446744073709551616.0) { // 2^64
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    return static_cast<unsigned long long>(value);
}

extern "C" unsigned int __cvt_fp2unsigned(double value) {
    if (value != value) { // NaN
        return 0;
    }
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 4294967296.0) { // 2^32
        return 0xFFFFFFFFu;
    }
    return static_cast<unsigned int>(value);
}
