#ifndef GXVERT_H
#define GXVERT_H

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Changes vs. upstream: ONLY the `#else` (non-Metrowerks) declaration of
// GXWGFifo — upstream defines it as a global variable (`volatile PPCWGPipe
// GXWGFifo;`), which is a definition in every translation unit that includes
// this header → "multiple definition" link errors on host toolchains. Ours
// is `extern`; compat/gx (M5) will provide the real definition. Everything
// else (the __GXCDEF* macros and the GX* immediate-vertex writers) is
// byte-for-byte identical to upstream.
//
// The member-access macros paste an underscore before the type name
// (`GXWGFifo._##td` → `GXWGFifo._u8`) to match the renamed PPCWGPipe
// members (see PPCWGPipe.h).
//
// TODO(PC_PORT, M5): these static-inline functions implement the PPC FIFO
// write path; compat/gx will turn the GX immediate API into native vertex
// capture and these inline bodies will be replaced by real implementations.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

#include <revolution/base/PPCWGPipe.h>

#ifdef __MWERKS__
volatile PPCWGPipe GXWGFifo : 0xCC008000;
#else
extern volatile PPCWGPipe GXWGFifo;
#endif

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
__GXCDEFX(GXNormal1x8, 1, u8)
__GXCDEFX(GXTexCoord1x8, 1, u8)

#ifdef __cplusplus
}
#endif

#endif  // GXVERT_H
