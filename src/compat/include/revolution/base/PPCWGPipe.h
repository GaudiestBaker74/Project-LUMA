#ifndef __PPCWGPIPE_H__
#define __PPCWGPIPE_H__

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream: the union members are renamed `_u8/_u16/...` on host
// toolchains. The upstream names (`u8 u8;` etc.) are rejected by GCC/Clang
// in C++ ("declaration changes meaning of 'u8'"); MSVC accepts them, but
// keeping one spelling everywhere avoids divergence. Our GXVert.h override
// pastes the underscore into the FIFO access (`GXWGFifo._##td`), so this
// matches exactly.
//
// The union models the PPC write-gather pipe (a hardware register). Nothing
// in the compiled game code touches it yet; compat/gx (M5) reimplements the
// GX FIFO entirely.
// ============================================================================

#ifdef  __cplusplus
extern  "C" {
#endif

#include <revolution/types.h>

typedef union uPPCWGPipe {
#ifdef __MWERKS__
    u8  u8;
    u16 u16;
    u32 u32;
    u64 u64;
    s8  s8;
    s16 s16;
    s32 s32;
    s64 s64;
    f32 f32;
    f64 f64;
#else
    u8  _u8;
    u16 _u16;
    u32 _u32;
    u64 _u64;
    s8  _s8;
    s16 _s16;
    s32 _s32;
    s64 _s64;
    f32 _f32;
    f64 _f64;
#endif
} PPCWGPipe;

#ifdef  __cplusplus
}
#endif

#endif  //__PPCWGPIPE_H__
