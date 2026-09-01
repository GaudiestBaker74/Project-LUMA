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
