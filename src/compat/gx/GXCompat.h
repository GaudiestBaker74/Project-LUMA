#pragma once
// =============================================================================
// compat/gx — M3 "first GX smell".
//
// A minimal, honest subset of the GX API that drives the demo through the
// real call path the game uses: GX* -> Platform::Renderer. It deliberately does
// NOT emulate the GX state machine yet — that is Milestone 5 (docs/gx.md).
// The point of this file in M3 is to prove the plumbing (GX call sites that
// main.cpp writes like the game would) reaches the Vulkan backend and to
// expose the first place where the M5 GX state mirror will live.
//
// Signatures follow the Revolution SDK where it declares them (GXInit,
// GXSetViewport); GXClearColor/GXClear are not present in the vendored
// headers (only the decompiled subset), so their standard signatures are
// declared here with SDK types (GXColor from GXStruct.h).
//
// TODO(PC_PORT, M5): replace with the real GX implementation (state mirror +
// command list) as the game code is ported; this file currently ignores most
// arguments on purpose and logs at TRACE.
// =============================================================================

#include <revolution/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// SDK types we reuse:
typedef struct _GXColor {
    u8 r, g, b, a;
} GXColor;

// --- M3 subset --------------------------------------------------------------

// Initializes the GX command FIFO. In M3 the renderer already owns the
// Vulkan state; this is a no-op that validates the entry point exists.
void GXInit(void* fifoPtr, u32 fifoSize);

// Sets the render target viewport (in GX pixels). Drives Renderer::setViewport.
void GXSetViewport(f32 x, f32 y, f32 w, f32 h, f32 nearZ, f32 farZ);

// Sets the clear color used by the next GXClear. Drives Renderer::setClearColor.
void GXClearColor(GXColor color);

// Clear masks (subset of the GXFBClr bits — simplified for M3).
enum {
    GX_CLEAR_COLOR = 0x1,
    GX_CLEAR_Z     = 0x2,
};

// Clears the framebuffer next frame with the current clear color (and depth,
// which M3 ignores — there is no depth buffer yet).
void GXClear(u32 clrMask);

#ifdef __cplusplus
}
#endif
