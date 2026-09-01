#ifndef GXVERT_H
#define GXVERT_H

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Changes vs. upstream:
//   1. `GXWGFifo` is NOT defined here — upstream defines it as a global
//      variable (`volatile PPCWGPipe GXWGFifo;`), which is a definition in
//      every TU that includes this header → "multiple definition" link
//      errors on host toolchains. Ours is a single `extern` to the simulated
//      write-gather pipe (GXCompatFifo.h), defined in compat/gx/GXCompat.cpp.
//   2. The pipe is `GXFifoPipe` (word objects with capturing assignment)
//      instead of the raw `PPCWGPipe` union: each `GXWGFifo._u8 = x` write
//      from the macros below feeds the M5.1 GX state machine, which
//      reconstructs vertices from the write stream exactly like the PPC
//      vertex processor does (using the current GX_VCD/GX_VAT).
//
// Everything else (the __GXCDEF* macros and the GX* immediate-vertex
// writers) is byte-for-byte identical to upstream, EXCEPT the 1x8/1x16 index
// writers below: upstream only vendored the 1x8 set (and no GXColor1x*), but
// the game also emits 16-bit indices (GX_INDEX16) and indexed colors — these
// are the standard RVL_SDK emitters, added back verbatim with the same macro.
//   3. Added (standard SDK API, missing from the vendored copy):
//      GXPosition1x16, GXNormal1x16, GXColor1x8, GXColor1x16, GXTexCoord1x16.
//
// The member-access macros paste an underscore before the type name
// (`GXWGFifo._##td` → `GXWGFifo._u8`) to match the GXFifoPipe member names.
// ============================================================================

// The simulated write-gather pipe (declared in GXCompatFifo.h, defined in
// compat/gx/GXCompat.cpp). Included outside extern "C": GXFifoPipe is a C++
// type with member functions.
#include <compat/gx/GXCompatFifo.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <revolution/base/PPCWGPipe.h>

#define __GXCDEF(prfx, n, t) __GXCDEF##n(prfx##n##t, t, t)
#define __GXCDEFX(func, n, t) __GXCDEF##n(func, t, t)

#define __GXCDEF1(func, ts, td)                                                                                                                      \
    static void func(const ts x) {                                                                                                                   \
        GXWGFifo._##td = (td)x;                                                                                                                         \
        return;                                                                                                                                      \
    }

#define __GXCDEF2(func, ts, td)                                                                                                                      \
    static void func(const ts x, const ts y) {                                                                                                       \
        GXWGFifo._##td = (td)x;                                                                                                                         \
        GXWGFifo._##td = (td)y;                                                                                                                         \
        return;                                                                                                                                      \
    }

#define __GXCDEF3(func, ts, td)                                                                                                                      \
    static void func(const ts x, const ts y, const ts z) {                                                                                           \
        GXWGFifo._##td = (td)x;                                                                                                                         \
        GXWGFifo._##td = (td)y;                                                                                                                         \
        GXWGFifo._##td = (td)z;                                                                                                                         \
        return;                                                                                                                                      \
    }

#define __GXCDEF4(func, ts, td)                                                                                                                      \
    static void func(const ts x, const ts y, const ts z, const ts w) {                                                                               \
        GXWGFifo._##td = (td)x;                                                                                                                         \
        GXWGFifo._##td = (td)y;                                                                                                                         \
        GXWGFifo._##td = (td)z;                                                                                                                         \
        GXWGFifo._##td = (td)w;                                                                                                                         \
        return;                                                                                                                                      \
    }

__GXCDEF(GXCmd, 1, u8)
__GXCDEF(GXCmd, 1, u16)
__GXCDEF(GXCmd, 1, u32)
__GXCDEF(GXCmd, 1, f32)

__GXCDEF(GXPosition, 3, f32)
__GXCDEF(GXPosition, 3, u8)
__GXCDEF(GXPosition, 3, s8)
__GXCDEF(GXPosition, 3, u16)
__GXCDEF(GXPosition, 3, s16)

__GXCDEF(GXPosition, 2, f32)
__GXCDEF(GXPosition, 2, u8)
__GXCDEF(GXPosition, 2, s8)
__GXCDEF(GXPosition, 2, u16)
__GXCDEF(GXPosition, 2, s16)

__GXCDEF(GXNormal, 3, f32)

__GXCDEF(GXColor, 1, u32)
__GXCDEF(GXColor, 4, u8)

__GXCDEF(GXTexCoord, 2, u8)
__GXCDEF(GXTexCoord, 2, u16)
__GXCDEF(GXTexCoord, 2, s16)
__GXCDEF(GXTexCoord, 2, f32)

__GXCDEFX(GXPosition1x8, 1, u8)
__GXCDEFX(GXPosition1x16, 1, u16)
__GXCDEFX(GXNormal1x8, 1, u8)
__GXCDEFX(GXNormal1x16, 1, u16)
__GXCDEFX(GXColor1x8, 1, u8)
__GXCDEFX(GXColor1x16, 1, u16)
__GXCDEFX(GXTexCoord1x8, 1, u8)
__GXCDEFX(GXTexCoord1x16, 1, u16)

#ifdef __cplusplus
}
#endif

#endif  // GXVERT_H
