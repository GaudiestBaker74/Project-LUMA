#pragma once
// =============================================================================
// compat/gx — channel lighting state (M5.7a) internal interface.
//
// GXLight.cpp mirrors the GX lighting state (GXSetNumChans/GXSetChanCtrl/
// GXSetChanMatColor/GXSetChanAmbColor/GXLoadLightObjImm + the position/normal
// matrices GXLoadPosMtxImm/GXLoadNrmMtxImm/GXSetCurrentMtx) and evaluates the
// GX per-vertex lighting formula on the CPU. The result replaces the vertex
// color in the fixed TEV layout (RASC/APREV of the TEV fragment shader), so
// the shader pipeline is untouched.
//
// The evaluator reproduces the hardware formula exactly as Dolphin does
// (VideoCommon/LightingShaderGen.cpp): 4 channels (COLOR0, COLOR1, ALPHA0,
// ALPHA1 — the GXChannelID & 3 slot order used by the SDK), per-channel
// material/ambient sources, light mask, diffuse function (NONE/SIGN/CLAMP)
// and attenuation function (NONE/SPOT/SPEC), and the fixed-point final
// multiply out = (mat * (lacc + (lacc >> 7))) >> 8 with lacc clamped 0..255.
//
// Coordinates: lights are specified in VIEW space (the game transforms them by
// the camera matrix before GXLoadLightObjImm); the vertex position/normal must
// be transformed by the current pos/nrm matrix before calling the evaluator.
// =============================================================================

#include <cstdint>

namespace Platform::CompatGx {

// One of the 4 channel slots (index = GXChannelID & 3, i.e. the SDK order
// COLOR0=0, COLOR1=1, ALPHA0=2, ALPHA1=3). Field semantics mirror the XF
// color-channel control register (GXSetChanCtrl). diffFn/attnFn store the
// effective values the SDK packs: attn SPEC forces diffuse NONE.
struct ChanLightState {
    std::int32_t enable = 0;   // GXBool (0/1)
    std::int32_t ambSrc = 0;   // GXColorSrc: GX_SRC_REG=0, GX_SRC_VTX=1
    std::int32_t matSrc = 0;   // GXColorSrc
    std::uint32_t lightMask = 0;  // GX_LIGHT0..7 bits (1 << i)
    std::int32_t diffFn = 0;   // GXDiffuseFn: GX_DF_NONE=0, SIGN=1, CLAMP=2
    std::int32_t attnFn = 0;   // GXAttnFn: GX_AF_SPEC=0, SPOT=1, NONE=2
};

// One light object, as read from the GXLightObj struct (GXLightObjInt layout).
struct LightParams {
    std::uint8_t color[4] = {};  // RGBA (8-bit)
    float a[3] = {};             // cos attenuation (spot) coefficients
    float k[3] = {};             // dist attenuation coefficients
    float pos[3] = {};           // view-space position
    float dir[3] = {};           // spot/spec direction (stored, i.e. negated input)
};

// Pure evaluator — no global state. chan[4] in slot order above; amb/mat are
// the ambient/material colors per channel pair (index 0 = COLOR0A0, 1 =
// COLOR1A1), 8-bit. clr0/clr1 are the raw vertex colors (0..1 floats).
// posView/nrmView are the transformed vertex position/normal in view space
// (nrmView normalized). Outputs the lit colors (0..1) for channel pairs 0/1.
void computeChannelLighting(const ChanLightState chan[4], const std::uint8_t amb[2][4],
                            const std::uint8_t mat[2][4], const LightParams lights[8],
                            const float posView[3], const float nrmView[3],
                            const float clr0[4], const float clr1[4], float out0[4],
                            float out1[4]);

// --- mirror accessors (GXLight.cpp) ------------------------------------------

// Resets the lighting mirror to the GXInit defaults (see GXInit.c): 0 chans,
// both channels disabled with SRC_REG amb / SRC_VTX mat, black amb, white mat,
// identity pos/nrm matrices, current matrix 0, lights cleared.
void resetLightingState();

// Returns the current pos matrix (12 floats, 3x4 row-major) and normal matrix
// (9 floats, 3x3 row-major) selected by GXSetCurrentMtx.
const float* currentPosMtx();
const float* currentNrmMtx();

// Returns the matrix slot by GX_PNMTX id (0, 3, 6, ... 27 — the value stored
// in MATINDEX_A / the PNMTXIDX vertex attribute). Identical storage to
// currentPosMtx()/currentNrmMtx() (which select by the current index), so the
// per-vertex skinning path can pick the right matrix per vertex.
const float* posMtxAt(int id);
const float* nrmMtxAt(int id);

// --- display-list mirror hooks (M5.7a) ---------------------------------------
//
// The DL interpreter routes XF matrix-array writes (command 0x10) and indexed
// XF loads (0x20..0x38, LOADINDX) into the same mirror the API setters use.
// XF address map: pos matrices 0x000-0x0FF (12 regs per matrix, matrix id at
// 4*id), normal matrices 0x400-0x45F (9 regs per matrix, id at 0x400+3*id),
// lights 0x600-0x67F (16 regs per light).

// One XF register write into the pos/nrm matrix arrays (bit pattern of a
// float). The address selects the matrix id and the offset inside it.
void dlXfPosReg(std::uint32_t xfAddr, float value);
void dlXfNrmReg(std::uint32_t xfAddr, float value);

// One XF register write into a light object (0x600 + light*16 + regOff):
// reg 0 = packed RGBA color, 1..3 = a (cos attn), 4..6 = k (dist attn),
// 7..9 = lpos, 10..12 = ldir (same layout GXLoadLightObjImm reads).
void dlLightReg(int lightIdx, int regOff, float value);

// LOADINDX payload: copies `size` floats from the CP matrix array (already
// resolved by the interpreter) into the XF matrix mirror at `xfAddr`.
void dlCopyMtxRegs(std::uint32_t xfAddr, int size, const float* data);

// CP MATINDEX_A write (register 0x30, GXSetCurrentMtx's CP image): the low 6
// bits are the position/normal matrix index (GX_PNMTX values).
void dlMatIdxA(std::uint32_t value);

// XF channel-control register writes (0x1009, 0x100A/0x100B, 0x100C/0x100D,
// 0x100E..0x1011) — the display-list image of GXSetNumChans, the amb/mat
// colors and GXSetChanCtrl. Registers are the raw XF u32s; the decode
// reproduces the SDK's packing (see GXLight.cpp).
void dlSetNumChans(std::uint32_t v);
void dlChanCtrlReg(int slot, std::uint32_t v);
void dlAmbColorReg(int pair, std::uint32_t v);
void dlMatColorReg(int pair, std::uint32_t v);

// Applies the mirrored channel-lighting state to one vertex: `posView`/
// `nrmView` are already in view space; the raw vertex colors clr0/clr1 (0..1)
// are replaced by the lit colors in out0/out1 (0..1).
void applyChannelLighting(const float posView[3], const float nrmView[3],
                          const float clr0[4], const float clr1[4], float out0[4],
                          float out1[4]);

// Builds the shader MVP for the current pos matrix and the GX projection
// (`proj` row-major 4x4, as stored by GXSetProjection). GX is row-vector:
// clip = posView * proj = pos * (posMtx * proj); the shader multiplies
// column-vectors, so the pushed matrix is the transpose of (posMtx * proj).
// With the identity pos matrix the result is proj^T (the identity case keeps
// the historical passthrough exact).
void buildMvp(const float posMtx3x4[12], const float proj[16], float outMvp[16]);

// Debug accessor for tests/dump (mirror of the channel state).
struct GxChanDebugState {
    int numChans = 0;
    int enable[4] = {};
    int ambSrc[4] = {};
    int matSrc[4] = {};
    std::uint32_t lightMask[4] = {};
    int diffFn[4] = {};
    int attnFn[4] = {};
    std::uint8_t ambColor[2][4] = {};
    std::uint8_t matColor[2][4] = {};
    int curPosMtx = 0;
    int curNrmMtx = 0;
};
void getChanDebugState(GxChanDebugState& out);

}  // namespace Platform::CompatGx
