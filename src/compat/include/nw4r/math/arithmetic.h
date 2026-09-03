#pragma once
// =============================================================================
// PC_PORT PATCH of the vendored libs/nw4r/include/nw4r/math/arithmetic.h
// (see src/compat/patches/README.md).
//
// Upstream FAbs/FSelect use MWERKS `register f32` parameters + `__asm {fabs}`
// / `asm {fsel}` blocks — invalid C++ on host toolchains (GCC/Clang/MSVC all
// reject `register` storage on parameters in C++17 and the MWERKS asm syntax).
//
// Change vs. upstream: scalar/std equivalents with identical semantics.
//   - fabs  -> std::fabs (clears the sign bit, same as the PPC fabs).
//   - fsel  -> (cond >= 0 || NaN(cond)?) : the PPC fsel selects ifPos when
//     cond >= +0.0 and ifNeg otherwise (including cond = -0.0/NaN -> ifNeg);
//     `cond >= 0.0f` is false for -0.0? No: -0.0 >= 0.0 is TRUE, and fsel
//     with -0.0 also selects ifPos (fsel treats -0.0 as >= 0), so the plain
//     comparison matches; NaN >= 0.0 is false -> ifNeg, same as fsel.
//   - U16ToF32/F32ToU16 keep using the OSFastCast macros (compat override
//     provides scalar versions).
//
// Everything else is identical to upstream.
// =============================================================================

#include <cmath>

#include <revolution/os.h>
#include <revolution/types.h>

namespace nw4r {
    namespace math {
        inline f32 U16ToF32(u16 x) {
            f32 rval;
            OSu16tof32(&x, &rval);
            return rval;
        }

        inline u16 F32ToU16(f32 x) {
            u16 rval;
            OSf32tou16(&x, &rval);
            return rval;
        }

        inline f32 FAbs(f32 x) {
            return std::fabs(x);
        }

        inline f32 FSelect(f32 cond, f32 ifPos, f32 ifNeg) {
            return (cond >= 0.0f) ? ifPos : ifNeg;
        }
    };  // namespace math
};  // namespace nw4r
