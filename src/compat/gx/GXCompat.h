#pragma once
// =============================================================================
// compat/gx — GX state mirror + immediate-vertex path (M5.1).
//
// M5.1 implements the "state -> pipeline" model of docs/gx.md for immediate
// vertices: the GX configuration calls maintain a mirror of the GX state
// (vertex descriptor GX_VCD, attribute formats GX_VAT, viewport/scissor/
// projection, cull/blend), and GXBegin/GXEnd + the immediate vertex writers
// (GXPosition3f32, GXColor4u8, ... — declared in the vendored GXVert.h
// header) capture vertices through the simulated write-gather pipe
// (GXCompatFifo.h) exactly like the PPC vertex processor does. When a
// primitive is complete, it is submitted to Platform::Renderer.
//
// Scope notes (M5.1):
//   * The vertex stream is serialized to float arrays (CPU-converted from the
//     GX comp types); the vertex shader consumes position + color0. Normal
//     and texture coordinates are captured (they are part of the VCD) but not
//     yet rendered (shaders for them arrive with M5.3/M5.7).
//   * GX_QUADS is decomposed into two triangles on the CPU (Vulkan has no
//     quad topology).
//   * Textures/TEV/lighting/display lists are later sub-milestones (M5.2+).
//
// Signatures use the real Revolution SDK enums/types from the vendored
// headers (GXEnum.h, GXGeometry.h, ...) so game call sites compile unchanged.
// GXClearColor/GXClear are not declared in the vendored subset, so their
// standard signatures are declared here.
// =============================================================================

#include <revolution/types.h>
#include <cstddef>
#include <revolution/gx/GXEnum.h>
// Vendored SDK types we reuse verbatim: GXStruct.h defines GXColor/GXColorS10/
// GXTexObj/..., GXFifo.h defines GXFifoObj (needed by GXInit.h).
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXInit.h>
#include <revolution/gx/GXGeometry.h>
// The immediate-vertex writers (GXPosition3f32, GXColor4u8, ...): declared in
// the vendored GXVert.h (PC_PORT override) and implemented through the
// simulated write-gather pipe.
#include <revolution/gx/GXVert.h>
#include <revolution/gx/GXTransform.h>
#include <revolution/gx/GXPixel.h>
// Channel lighting (M5.7a): GXSetNumChans/GXSetChanCtrl/GXSetChanMatColor/
// GXSetChanAmbColor/GXLoadLightObjImm + the position/normal matrices
// (GXLoadPosMtxImm/GXLoadNrmMtxImm/GXSetCurrentMtx) — implemented in
// GXLight.cpp (the GXInitLight* builders come from the patched GXLight.c).
#include <revolution/gx/GXLighting.h>
#include "compat/gx/GXLightInternal.h"
// Texture objects + texgen (M5.3): GXInitTexObj/LOD/CI/Tlut, GXLoadTexObj,
// GXSetTexCoordGen2, GXLoadTexMtxImm — implemented in GXTexture.cpp.
#include <revolution/gx/GXTexture.h>
// M5.7c: the EFB copy APIs (GXCopyDisp/GXCopyTex/GXSetCopyClear/... and the
// GXRenderModeObj externs) — declared by the vendored GXFrameBuf.h (pulls
// revolution/gx.h, the full SDK header set; the compat layer implements the
// subset the game calls).
#include <revolution/gx/GXFrameBuf.h>
// TEV (M5.4): GXSetTevOp/ColorIn/ColorOp/Color/Order/KColor/SwapMode... —
// implemented in GXTev.cpp. The internal header also exposes the UBO layout
// and the CPU reference evaluator (tests).
#include <revolution/gx/GXTev.h>
// Display lists (M5.6): GXCallDisplayList — implemented in GXCompat.cpp.
#include <revolution/gx/GXDispList.h>
#include "compat/gx/GXTevInternal.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- state configuration (M5.1) --------------------------------------------

// (GXInit and GXSetViewport are declared by the vendored GXInit.h/
// GXTransform.h — included above.)

// Sets the scissor rectangle in GX pixels.
void GXSetScissor(u32 x, u32 y, u32 w, u32 h);

// Stores the projection matrix (already in clip space, GX convention) used as
// the MVP by the GX vertex shader. `type` is GX_ORTHOGRAPHIC/GX_PERSPECTIVE.
void GXSetProjection(const f32 mtx[4][4], GXProjectionType type);

// Cull mode (GX_CULL_NONE/BACK/FRONT/ALL) -> PipelineDesc::cullMode (M5.5).
void GXSetCullMode(GXCullMode mode);

// Blend mode + factors + logic op (M5.5): GX_BM_BLEND enables blending with
// src/dst factors, GX_BM_SUBTRACT blends dst − src, GX_BM_LOGIC uses the
// logic op. GXSetDstAlpha remaps the DSTALPHA factors to the constant alpha.
void GXSetBlendMode(GXBlendMode mode, GXBlendFactor srcFactor, GXBlendFactor dstFactor,
                    GXLogicOp op);

// Pixel-engine state (M5.5) — all declared by the vendored GXPixel.h and
// implemented in GXCompat.cpp: GXSetZMode(compare_enable, func, update_enable)
// -> depth test/compare/write; GXSetZCompLoc (mirrored; affects Z-texture
// emulation in M5.7); GXSetColorUpdate / GXSetAlphaUpdate -> color write
// masks; GXSetDstAlpha -> CONSTANT_ALPHA blend factor; GXSetDither /
// GXSetPixelFmt are mirrored (the PC EFB is RGBA8, no dithering/YUV —
// M5.7).

// Sets the clear color used by the next GXClear. Drives Renderer::setClearColor.
void GXClearColor(GXColor color);

// Clear masks (subset of the GXFBClr bits).
enum {
    GX_CLEAR_COLOR = 0x1,
    GX_CLEAR_Z     = 0x2,
};

// Clears the current render target next frame with the current clear color.
void GXClear(u32 clrMask);

// --- vertex descriptor / formats (declared by vendored GXGeometry.h) --------
//   void GXSetVtxDesc(GXAttr attr, GXAttrType type);   // DIRECT / INDEX8/16
//   void GXClearVtxDesc(void);
//   void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt,
//                        GXCompType type, u8 frac);
//   void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list);
//   void GXBegin(GXPrimitive prim, GXVtxFmt vtxfmt, u16 nverts);
//   void GXEnd(void);   // static inline no-op (vendored) — the vertex count
//                       // from GXBegin completes the primitive.
//
// M5.2 — indexed attributes: with GX_INDEX8/GX_INDEX16 in the VCD, the
// stream carries one index per attribute (written with the GXPosition1x8/
// GXPosition1x16/... emitters) and the attribute data is fetched from the
// array registered with GXSetArray(attr, base, stride), converting per the
// VAT (component count, comp type, frac) exactly like DIRECT data.
//
// M5.3 — textures: GXInitTexObj/GXInitTexObjLOD/GXLoadTexObj manage texture
// objects (raw GX tiled data -> RGBA8 via Bti.h -> renderer texture, created
// lazily). GXLoadTexObj binds TEXMAP0..7; M5.3 consumes TEXMAP0 in the
// textured GX shaders (texel * vertex color). GXSetTexCoordGen2 + texgen
// matrices (GXLoadTexMtxImm) are resolved per-vertex on the CPU; BUMP*/SRTG
// and TLUT management are TODO (M5.7).

// --- display lists (M5.6, declared by the vendored GXDispList.h) -----------
//   void GXCallDisplayList(const void* list, u32 nbytes);   // replay a DL
// The interpreter walks the console FIFO opcode stream byte-exactly:
//   * CP (0x08) VCD/VAT/array-base/stride commands update the same vertex
//     descriptor/format/array mirrors GXSetVtxDesc/GXSetVtxAttrFmt/GXSetArray
//     drive — a DL can bake its vertex format.
//   * XF (0x10) texgen registers (0x1040..0x1057) update the texgen mirror.
//   * BP (0x61) registers update the TEV/PE mirrors (GEN_MODE, ZMODE,
//     BLENDMODE, CONSTANTALPHA, TREF, TEVC/TEVA, TEV color/constant
//     registers, KSEL + swap tables, fog + alpha compare).
//   * Primitives (0x80..0xBF) feed the same vertex machinery as GXBegin
//     (no external GXBegin needed; `vat` = the primitive's vertex format).
// PC limitations: CP ARRAY_BASE carries a 32-bit *physical* address — it is
// resolved through a registry populated by GXSetArray (GX_PHY_ADDR key); the
// 0x40 CALL_DL opcode (a 32-bit address) is not translatable on PC — the
// game nests lists from C++ with GXCallDisplayList directly.
// GXBeginDisplayList/GXEndDisplayList (the RVLFaceLib recording path) are not
// needed: J3D records DLs into its own buffers with the GD writers.

// --- internal hooks (PC_PORT) — implemented in GXTexture.cpp ---------------
// Forward-declared here (outside extern "C") so GXCompat.cpp can call them
// without depending on Platform::Renderer types in this header (void*).
namespace Platform::CompatGx {
// Applies the GXSetTexCoordGen2 generator of `coordId` (GX_TEXCOORD0..7) to
// the current vertex attributes (in/out sCurAttr), transforming TEX coords.
void resolveTexGen(int coordId, float attrs[GX_VA_MAX_ATTR][4]);
// Returns the texture/sampler currently bound to TEXMAP0 (or nulls).
void getTexMap0(void** outTex, void** outSam);
// Returns the 8 TEXMAP0..7 textures/samplers (nulls for unbound slots). M5.4
// (TEV) binds the whole array per draw.
void getTexMaps(void** outTex, void** outSam);
// Returns the texel size (w, h) of each bound TEXMAP0..7 (0 for unbound or
// empty slots). M5.7b (indirect stages) normalizes the warp offset by the
// direct map's size.
void getTexMapDims(float outDims[8][2]);
// Resets the GX texture state (texgen, tex matrices, texmap bindings) —
// called from GXInit (mirrors the console register reset).
void resetTextureState();
// Releases renderer resources owned by texture objects (GXCompatShutdown).
void shutdownTextures();
// Applies an XF texgen-control register pair to the texgen mirror (M5.6 DL
// interpreter): `xfTex` is the XF_TEXn value, `xfDual` the XF_DUALTEXn value.
// Only the regular (non-bump) texgen types are decoded; the main matrix id is
// a GX-side parameter not stored in the XF registers and keeps its value.
void dlApplyXfTexGen(int coordId, std::uint32_t xfTex, std::uint32_t xfDual);

// M5.7c (copy EFB -> swapchain / texture) — implemented in GXCopy.cpp.
// Returns the EFB render target (created lazily, sized by the current
// GXSetDispCopySrc, default 640x448, color format = the swapchain format so
// the GX pipelines built in flushDraw match the attachment). Null when the
// renderer is not initialized. Called by the frame host before beginPass().
void* getEfbRenderTarget();
// Releases the EFB render target (called from GXCompatShutdown).
void shutdownEfb();
// Encodes a w*h RGBA8 buffer into the GX tiled image format `gxFmt`
// (GX_TF_I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR), writing the full mip chain
// when `mipmap`. `outBytes` must be >= encodedTexSize(). Returns false for
// unsupported formats or sizes (w/h must be multiples of 4, of 8 for CMPR).
bool encodeEfbRgbaToGx(const std::uint8_t* rgba, std::uint32_t w,
                       std::uint32_t h, std::uint8_t gxFmt, bool mipmap,
                       std::uint8_t* out, size_t outBytes);
// Size of the GX tiled image (base level + full mip chain when `mipmap`).
// Matches the layout btiDecodeToRgba8 expects.
size_t encodedTexSize(std::uint32_t w, std::uint32_t h, std::uint8_t gxFmt,
                      bool mipmap);
}

// --- debug / integration hooks (PC_PORT) -------------------------------------

// Called by main.cpp AFTER Renderer::endFrame(): resets the dynamic vertex
// buffer cursor for the next frame (the backing buffer is reused; one frame
// in flight and endFrame() waits the fence, so rewriting is safe).
void GXCompatEndFrame();

// Called by main.cpp before Renderer::shutdown(): releases the dynamic vertex
// buffer and the renderer textures owned by the GX path.
void GXCompatShutdown();

// --- debug / tests -----------------------------------------------------------

// Snapshot of the pixel-engine state mirror (M5.5) for tests/dump. The blend/
// z/cull/write-mask/dst-alpha state drives PipelineDesc at draw time; this
// accessor pins the GX API -> mirror mapping headlessly.
struct GxPeDebugState {
    int cullMode;         // GXCullMode
    int blendMode;        // GXBlendMode
    int blendSrc;         // GXBlendFactor
    int blendDst;         // GXBlendFactor
    int blendLogicOp;     // GXLogicOp
    int zTest;            // GXSetZMode compare_enable
    int zFunc;            // GXCompare
    int zWrite;           // GXSetZMode update_enable
    int zCompLoc;         // GXSetZCompLoc
    int colorUpdate;      // GXSetColorUpdate
    int alphaUpdate;      // GXSetAlphaUpdate
    int dstAlphaEnable;   // GXSetDstAlpha
    int dstAlphaValue;    // u8 constant alpha
    int dither;           // GXSetDither
    int pixelFmt;         // GXPixelFmt
    int zFormat;          // GXZFmt16
};
void GXCompatDebugPeState(GxPeDebugState& out);

// M5.7c: snapshot of the copy/EFB state mirror for tests/dump.
struct GxCopyDebugState {
    u16 dispSrcX, dispSrcY, dispSrcW, dispSrcH;   // GXSetDispCopySrc
    u16 dispDstW, dispDstH;                       // GXSetDispCopyDst
    u16 texSrcX, texSrcY, texSrcW, texSrcH;       // GXSetTexCopySrc
    u16 texDstW, texDstH;                         // GXSetTexCopyDst
    int texFmt;                                   // GXTexFmt
    int texMipmap;                                // GXBool
    u8 copyClearR, copyClearG, copyClearB, copyClearA;  // GXSetCopyClear color
    u32 copyClearZ;                               // GXSetCopyClear zClear
    int copyClamp;                                // GXFBClamp (GXSetCopyClamp)
    int copyFilterAA;                             // GXBool (GXSetCopyFilter)
    int copyFilterVFilter;                        // GXBool
    int dispCopyGamma;                            // GXGamma (GXSetDispCopyGamma)
    int dispCopyFrame2Field;                      // GXCopyMode
    u32 dispCopyYScale;                           // GXSetDispCopyYScale return
    u32 efbW, efbH;                               // current EFB render target size (0 = none)
};
void GXCompatDebugCopyState(GxCopyDebugState& out);

// Debug: returns the vertex data captured by the most recent complete
// primitive (floats, stride = `outStride`). `count` = number of vertices.
// Used by unit tests and --dump-gx. Returns nullptr if no primitive yet.
const float* GXCompatDebugVertices(int* outCount, int* outStride);

#ifdef __cplusplus
}
#endif
