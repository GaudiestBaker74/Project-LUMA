// =============================================================================
// compat/gx — channel lighting, lights and position/normal matrices (M5.7a).
//
// Mirror + CPU evaluator for the GX lighting pipeline. The GX APIs here do not
// write hardware registers: they populate a semantic mirror that
// GXCompat.cpp::flushDraw consumes per vertex (position -> view space via the
// current pos matrix, normal -> view space via the current nrm matrix, then
// applyChannelLighting replaces the vertex color that the TEV shader reads as
// RASC/APREV). The pure evaluator (computeChannelLighting) is Dolphin-exact
// (VideoCommon/LightingShaderGen.cpp) and unit-testable headlessly.
//
// The GXInitLight* builders are provided by the patched GXLight.c
// (src/compat/patches/RVL_SDK/gx/GXLight.c); GXLoadLightObjImm reads the
// GXLightObj struct they fill and stores it in the mirror (the console writes
// XF registers instead).
//
// Channel slots are indexed by GXChannelID & 3 exactly like the SDK
// (gx->chanCtrl[]): 0=COLOR0, 1=COLOR1, 2=ALPHA0, 3=ALPHA1. GX_COLOR0A0 /
// GX_COLOR1A1 write both the color and alpha slot of a pair.
//
// Matrix conventions: GX is row-vector (PSMTXMultVec: dst[r] = src * m[r] +
// m[r][3]); pos matrices are 3x4, normal matrices are the 3x3 upper part.
// GXSetCurrentMtx(GX_PNMTXn) selects pos slot n and nrm slot n/3.
// =============================================================================

#include "compat/gx/GXLightInternal.h"

#include "compat/gx/GXCompat.h"

// GXLightObjInt (the layout of the caller-owned GXLightObj the GXInitLight*
// builders fill) + GX_SETUP_LIGHT.
#include <revolution/gx/GXTypes.h>

#include "platform/Log/Log.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace Platform::CompatGx {

namespace {

// Channel slots (index = GXChannelID & 3).
enum {
    kSlotColor0 = 0,
    kSlotColor1 = 1,
    kSlotAlpha0 = 2,
    kSlotAlpha1 = 3,
};

constexpr int kMaxLights = 8;
constexpr int kMaxMtx = 32;

ChanLightState sChan[4];
std::uint8_t sAmbColor[2][4] = {};
std::uint8_t sMatColor[2][4] = {};
LightParams sLights[kMaxLights];
int sNumChans = 0;

// 3x4 row-major (12 floats) / 3x3 row-major (9 floats).
float sPosMtx[kMaxMtx][12];
float sNrmMtx[kMaxMtx][9];
int sCurPosMtx = 0;
int sCurNrmMtx = 0;

// --- helpers --------------------------------------------------------------

int round255(float v) {
    // Dolphin: int(round(x * 255.0)) — half away from zero.
    return static_cast<int>(std::round(v * 255.0f));
}

// (mat * (lacc + (lacc >> 7))) >> 8 — the hardware fixed-point multiply that
// maps two 8-bit values to their product/255 with rounding.
int mul255(int mat, int lacc) { return (mat * (lacc + (lacc >> 7))) >> 8; }

}  // namespace

// --- pure evaluator (Dolphin-exact) ------------------------------------------

void computeChannelLighting(const ChanLightState chan[4], const std::uint8_t amb[2][4],
                            const std::uint8_t mat[2][4], const LightParams lights[8],
                            const float posView[3], const float nrmView[3],
                            const float clr0[4], const float clr1[4], float out0[4],
                            float out1[4]) {
    float* outPair[2] = {out0, out1};
    // The output alpha of pair p comes from the ALPHA slot (2 + p); the RGB
    // from the COLOR slot (0 + p). Process each slot and store its components.
    for (int j = 0; j < 4; ++j) {
        const bool isColor = (j == kSlotColor0 || j == kSlotColor1);
        const int pair = j & 1;
        const float* base = (pair == 0) ? clr0 : clr1;
        const ChanLightState& c = chan[j];

        // Material color: 8-bit int4 (vertex color or register).
        int matV[4];
        if (c.matSrc == 1 /* GX_SRC_VTX */) {
            for (int i = 0; i < 4; ++i) {
                matV[i] = round255(base[i]);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                matV[i] = mat[pair][i];
            }
        }

        // Accumulator: ambient color or the vertex color; 255 when the
        // channel is disabled (output = material color).
        int lacc[4];
        if (c.enable != 0) {
            if (c.ambSrc == 1 /* GX_SRC_VTX */) {
                for (int i = 0; i < 4; ++i) {
                    lacc[i] = round255(base[i]);
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    lacc[i] = amb[pair][i];
                }
            }

            for (int li = 0; li < kMaxLights; ++li) {
                if ((c.lightMask & (1u << li)) == 0) {
                    continue;
                }
                const LightParams& L = lights[li];

                // Per-light geometry: direction toward the light and the
                // attenuation (Dolphin AttenuationFunc cases).
                float ldir[3];
                float attn;
                switch (c.attnFn) {
                case 0 /* GX_AF_SPEC */: {
                    float d[3] = {L.pos[0] - posView[0], L.pos[1] - posView[1],
                                  L.pos[2] - posView[2]};
                    const float lenSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    if (lenSq == 0.0f) {
                        std::memcpy(ldir, nrmView, sizeof(ldir));
                    } else {
                        const float inv = 1.0f / std::sqrt(lenSq);
                        ldir[0] = d[0] * inv;
                        ldir[1] = d[1] * inv;
                        ldir[2] = d[2] * inv;
                    }
                    const float ndl = nrmView[0] * ldir[0] + nrmView[1] * ldir[1] +
                                      nrmView[2] * ldir[2];
                    // Gated by the normal facing the light, then the spec
                    // falloff uses the light direction.
                    const float attn0 = (ndl >= 0.0f)
                                            ? std::max(0.0f, nrmView[0] * L.dir[0] +
                                                                  nrmView[1] * L.dir[1] +
                                                                  nrmView[2] * L.dir[2])
                                            : 0.0f;
                    const float cosAttn = std::max(
                        0.0f, L.a[0] + L.a[1] * attn0 + L.a[2] * attn0 * attn0);
                    const float distAttn = L.k[0] + L.k[1] * attn0 + L.k[2] * attn0 * attn0;
                    attn = (distAttn != 0.0f) ? cosAttn / distAttn : 0.0f;
                    break;
                }
                case 1 /* GX_AF_SPOT */: {
                    float d[3] = {L.pos[0] - posView[0], L.pos[1] - posView[1],
                                  L.pos[2] - posView[2]};
                    const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    const float dist = std::sqrt(dist2);
                    if (dist != 0.0f) {
                        ldir[0] = d[0] / dist;
                        ldir[1] = d[1] / dist;
                        ldir[2] = d[2] / dist;
                    } else {
                        std::memcpy(ldir, nrmView, sizeof(ldir));
                    }
                    const float cosA =
                        std::max(0.0f, ldir[0] * L.dir[0] + ldir[1] * L.dir[1] +
                                           ldir[2] * L.dir[2]);
                    const float cosAttn = std::max(
                        0.0f, L.a[0] + L.a[1] * cosA + L.a[2] * cosA * cosA);
                    const float distAttn = L.k[0] + L.k[1] * dist + L.k[2] * dist2;
                    attn = (distAttn != 0.0f) ? cosAttn / distAttn : 0.0f;
                    break;
                }
                default /* GX_AF_NONE */: {
                    float d[3] = {L.pos[0] - posView[0], L.pos[1] - posView[1],
                                  L.pos[2] - posView[2]};
                    const float lenSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    if (lenSq == 0.0f) {
                        std::memcpy(ldir, nrmView, sizeof(ldir));
                    } else {
                        const float inv = 1.0f / std::sqrt(lenSq);
                        ldir[0] = d[0] * inv;
                        ldir[1] = d[1] * inv;
                        ldir[2] = d[2] * inv;
                    }
                    attn = 1.0f;
                    break;
                }
                }

                // Diffuse term: NONE multiplies by attn only; SIGN/CLAMP by
                // attn * dot(ldir, normal) (CLAMP floors at 0).
                const float ndl = nrmView[0] * ldir[0] + nrmView[1] * ldir[1] +
                                  nrmView[2] * ldir[2];
                float diffuse;
                switch (c.diffFn) {
                case 1 /* GX_DF_SIGN */:
                    diffuse = attn * ndl;
                    break;
                case 2 /* GX_DF_CLAMP */:
                    diffuse = attn * std::max(0.0f, ndl);
                    break;
                default /* GX_DF_NONE */:
                    diffuse = attn;
                    break;
                }

                // Accumulate the components this slot owns: RGB for color
                // slots, alpha for alpha slots (per Dolphin's swizzle).
                if (isColor) {
                    for (int comp = 0; comp < 3; ++comp) {
                        lacc[comp] += static_cast<int>(
                            std::round(diffuse * static_cast<float>(L.color[comp])));
                    }
                } else {
                    lacc[3] += static_cast<int>(
                        std::round(diffuse * static_cast<float>(L.color[3])));
                }
            }
        } else {
            lacc[0] = lacc[1] = lacc[2] = lacc[3] = 255;
        }

        // Clamp the accumulator to 0..255, then the fixed-point multiply.
        for (int i = 0; i < 4; ++i) {
            lacc[i] = std::clamp(lacc[i], 0, 255);
            lacc[i] = mul255(matV[i], lacc[i]);
        }

        // Store the components this slot owns.
        if (isColor) {
            outPair[pair][0] = lacc[0] / 255.0f;
            outPair[pair][1] = lacc[1] / 255.0f;
            outPair[pair][2] = lacc[2] / 255.0f;
        } else {
            outPair[pair][3] = lacc[3] / 255.0f;
        }
    }
}

// --- mirror ----------------------------------------------------------------

void resetLightingState() {
    for (int i = 0; i < 4; ++i) {
        ChanLightState& c = sChan[i];
        // GXInit: GXSetChanCtrl(GX_COLOR0A0/GX_COLOR1A1, DISABLE, SRC_REG,
        // SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE).
        c = ChanLightState{};
        c.enable = 0;
        c.ambSrc = 0;  // GX_SRC_REG
        c.matSrc = 1;  // GX_SRC_VTX
        c.diffFn = 0;  // GX_DF_NONE
        c.attnFn = 2;  // GX_AF_NONE
    }
    for (int p = 0; p < 2; ++p) {
        // Ambient black, material white (GXInit).
        std::memset(sAmbColor[p], 0, sizeof(sAmbColor[p]));
        std::memset(sMatColor[p], 0xFF, sizeof(sMatColor[p]));
    }
    std::memset(sLights, 0, sizeof(sLights));
    sNumChans = 0;
    for (int m = 0; m < kMaxMtx; ++m) {
        std::memset(sPosMtx[m], 0, sizeof(sPosMtx[m]));
        sPosMtx[m][0] = sPosMtx[m][5] = sPosMtx[m][10] = 1.0f;  // identity 3x4
        std::memset(sNrmMtx[m], 0, sizeof(sNrmMtx[m]));
        sNrmMtx[m][0] = sNrmMtx[m][4] = sNrmMtx[m][8] = 1.0f;   // identity 3x3
    }
    sCurPosMtx = 0;
    sCurNrmMtx = 0;
}

const float* currentPosMtx() { return sPosMtx[sCurPosMtx]; }
const float* currentNrmMtx() { return sNrmMtx[sCurNrmMtx]; }

const float* posMtxAt(int id) { return (id >= 0 && id < kMaxMtx) ? sPosMtx[id] : sPosMtx[0]; }
const float* nrmMtxAt(int id) {
    const int slot = id / 3;  // normal slot, like GXSetCurrentMtx/MATINDEX_A
    return (id >= 0 && slot < kMaxMtx) ? sNrmMtx[slot] : sNrmMtx[0];
}

// XF matrix-register writes from the DL interpreter. The address selects the
// slot and the register offset inside it; out-of-range writes (which would
// land in the light/post-matrix area) are dropped like the console's
// register-range guard (see XFStructs.cpp LoadXFReg).
void dlXfPosReg(std::uint32_t xfAddr, float value) {
    if (xfAddr < 0x100) {  // pos matrices: 0x000-0x0FF
        // XF matrix k lives at regs [4*GX_PNMTX_k, +12) = [12k, 12k+12) and
        // the mirror is indexed by the GX_PNMTX id (3k) exactly like
        // GXLoadPosMtxImm/GXSetCurrentMtx (J3D always uses aligned ids).
        const int slot = static_cast<int>(xfAddr) / 12;
        const int off = static_cast<int>(xfAddr) % 12;
        const int id = 3 * slot;
        if (id < kMaxMtx) {
            sPosMtx[id][off] = value;
        }
    }
}

void dlXfNrmReg(std::uint32_t xfAddr, float value) {
    if (xfAddr >= 0x400 && xfAddr < 0x460) {  // normal matrices: 0x400-0x45F
        const int rel = static_cast<int>(xfAddr) - 0x400;
        const int mtx = rel / 9;
        const int off = rel % 9;
        if (mtx < kMaxMtx) {
            sNrmMtx[mtx][off] = value;
        }
    }
}

void dlLightReg(int lightIdx, int regOff, float value) {
    if (lightIdx < 0 || lightIdx >= kMaxLights || regOff < 0 || regOff >= 16) {
        return;
    }
    // XF light register layout (GXLightObjImm writes via WriteLightObjPS):
    // regs 0-2 zero/reserved, reg 3 = packed RGBA color, 4..6 = a (cos
    // attenuation), 7..9 = k (dist attenuation), 10..12 = lpos, 13..15 = ldir.
    LightParams& L = sLights[lightIdx];
    if (regOff == 3) {
        const std::uint32_t c = std::bit_cast<std::uint32_t>(value);
        L.color[0] = static_cast<std::uint8_t>((c >> 24) & 0xFF);
        L.color[1] = static_cast<std::uint8_t>((c >> 16) & 0xFF);
        L.color[2] = static_cast<std::uint8_t>((c >> 8) & 0xFF);
        L.color[3] = static_cast<std::uint8_t>(c & 0xFF);
    } else if (regOff >= 4 && regOff <= 6) {
        L.a[regOff - 4] = value;
    } else if (regOff >= 7 && regOff <= 9) {
        L.k[regOff - 7] = value;
    } else if (regOff >= 10 && regOff <= 12) {
        L.pos[regOff - 10] = value;
    } else if (regOff >= 13 && regOff <= 15) {
        L.dir[regOff - 13] = value;
    }
}

// LOADINDX payload: a contiguous block of `size` floats from a CP matrix
// array goes straight into the XF matrix mirror at xfAddr (the console copies
// u32s, so bit-exact float values are preserved).
void dlCopyMtxRegs(std::uint32_t xfAddr, int size, const float* data) {
    for (int i = 0; i < size; ++i) {
        dlXfPosReg(xfAddr + static_cast<std::uint32_t>(i), data[i]);
        dlXfNrmReg(xfAddr + static_cast<std::uint32_t>(i), data[i]);
    }
}

// CP MATINDEX_A (0x30): the low 6 bits are the GX_PNMTX index shared by the
// pos and nrm matrices — the exact semantics of GXSetCurrentMtx (which is
// what the DL's matrix-index path lands in on the console).
void dlMatIdxA(std::uint32_t value) {
    const int id = static_cast<int>(value & 0x1F);
    sCurPosMtx = id;
    sCurNrmMtx = id / 3;
    PL_LOG_TRACE("gx", "CP MATINDEX_A 0x%03X -> pos %d nrm %d", value, id, id / 3);
}

// XF channel-control register (0x100E..0x1011, GXSetChanCtrl's XF image).
// Layout (xf_mem.h COLOR0CNTRL fields, identical for COLOR1/ALPHA0/ALPHA1):
//   bit 0 matSrc, bit 1 enable, bits 2-5 lights 0-3, bit 6 ambSrc,
//   bits 7-8 diffFn (GX_DF_NONE/SIGN/CLAMP), bit 9 attn enable, bit 10 attn
//   select, bits 11-14 lights 4-7. The SDK packs attn as (en,sel): SPEC=(0,0),
//   SPOT=(1,1), NONE=(0,1), which maps to the mirror's GXAttnFn values.
void dlChanCtrlReg(int slot, std::uint32_t v) {
    if (slot < 0 || slot >= 4) {
        return;
    }
    ChanLightState& c = sChan[slot];
    c.matSrc = static_cast<int>(v & 0x1);
    c.enable = static_cast<int>((v >> 1) & 0x1);
    c.lightMask = ((v >> 2) & 0xF) | (((v >> 11) & 0xF) << 4);
    c.ambSrc = static_cast<int>((v >> 6) & 0x1);
    c.diffFn = static_cast<int>((v >> 7) & 0x3);
    const int attnEn = static_cast<int>((v >> 9) & 0x1);
    const int attnSel = static_cast<int>((v >> 10) & 0x1);
    c.attnFn = (attnEn == 0 && attnSel == 0) ? static_cast<int>(GX_AF_SPEC)
                                             : (attnEn == 1 && attnSel == 1)
                                                   ? static_cast<int>(GX_AF_SPOT)
                                                   : static_cast<int>(GX_AF_NONE);
    PL_LOG_TRACE("gx", "XF chan ctrl slot %d: enable %d mat %d amb %d mask 0x%02X diff %d attn %d",
                 slot, c.enable, c.matSrc, c.ambSrc, c.lightMask, c.diffFn, c.attnFn);
}

// XF ambient/material color registers (0x100A/0x100B, 0x100C/0x100D): the raw
// GXColor u32 (R=MSB .. A=LSB) as GXSetChan*Color packs it.
void dlAmbColorReg(int pair, std::uint32_t v) {
    if (pair < 0 || pair >= 2) {
        return;
    }
    sAmbColor[pair][0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    sAmbColor[pair][1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    sAmbColor[pair][2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    sAmbColor[pair][3] = static_cast<std::uint8_t>(v & 0xFF);
}

void dlMatColorReg(int pair, std::uint32_t v) {
    if (pair < 0 || pair >= 2) {
        return;
    }
    sMatColor[pair][0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    sMatColor[pair][1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    sMatColor[pair][2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    sMatColor[pair][3] = static_cast<std::uint8_t>(v & 0xFF);
}

// XF SETNUMCHAN (0x1009, GXSetNumChans' XF image — the game also programs the
// channel count through BP GEN_MODE).
void dlSetNumChans(std::uint32_t v) {
    sNumChans = static_cast<int>(v & 0x3);
    PL_LOG_TRACE("gx", "XF SETNUMCHAN -> %d", sNumChans);
}

void applyChannelLighting(const float posView[3], const float nrmView[3],
                          const float clr0[4], const float clr1[4], float out0[4],
                          float out1[4]) {
    computeChannelLighting(sChan, sAmbColor, sMatColor, sLights, posView, nrmView, clr0,
                           clr1, out0, out1);
}

void buildMvp(const float posMtx3x4[12], const float proj[16], float outMvp[16]) {
    // 4x4 "rows-are-output" extension of the GX 3x4 pos matrix: its 3 rows
    // followed by (0,0,0,1). In this convention (GX storage) transforming is
    // out[r] = v * m[r] + m[r][3], so the composition of two GX matrices is
    // C = A * B (row-vector product), and the chain pos-then-proj is
    // clip = v * (posMtx * proj)... but the GX 3x4 posMtx maps to the
    // standard row-vector matrix R (out[c] = v * R[col c]) as R^T = pos44,
    // which puts the projection FIRST: f = proj * pos44 (verified numerically,
    // see the test). The shader multiplies column-vectors, so we push f^T.
    float pos44[16] = {};
    std::memcpy(pos44, posMtx3x4, 12 * sizeof(float));
    pos44[15] = 1.0f;
    float m[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) {
                v += proj[r * 4 + k] * pos44[k * 4 + c];
            }
            m[r * 4 + c] = v;
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            outMvp[c * 4 + r] = m[r * 4 + c];
        }
    }
}

void getChanDebugState(GxChanDebugState& out) {
    out.numChans = sNumChans;
    for (int i = 0; i < 4; ++i) {
        out.enable[i] = sChan[i].enable;
        out.ambSrc[i] = sChan[i].ambSrc;
        out.matSrc[i] = sChan[i].matSrc;
        out.lightMask[i] = sChan[i].lightMask;
        out.diffFn[i] = sChan[i].diffFn;
        out.attnFn[i] = sChan[i].attnFn;
    }
    std::memcpy(out.ambColor, sAmbColor, sizeof(sAmbColor));
    std::memcpy(out.matColor, sMatColor, sizeof(sMatColor));
    out.curPosMtx = sCurPosMtx;
    out.curNrmMtx = sCurNrmMtx;
}

}  // namespace Platform::CompatGx

// =============================================================================
// GX API surface (extern "C", declared by the vendored GXLighting.h /
// GXTransform.h) — implemented as mirror updates.
// =============================================================================

extern "C" {

void GXSetNumChans(u8 nChans) {
    using namespace Platform::CompatGx;
    sNumChans = nChans;
    PL_LOG_TRACE("gx", "GXSetNumChans(%u)", static_cast<unsigned>(nChans));
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                   u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    using namespace Platform::CompatGx;
    const int slot = static_cast<int>(chan) & 0x3;
    // The SDK packs the effective diffuse function: SPEC attenuation forces
    // GX_DF_NONE.
    const int effDiff = (attn_fn == GX_AF_SPEC) ? static_cast<int>(GX_DF_NONE)
                                                : static_cast<int>(diff_fn);
    auto apply = [&](int s) {
        sChan[s].enable = (enable != GX_FALSE) ? 1 : 0;
        sChan[s].ambSrc = static_cast<int>(amb_src);
        sChan[s].matSrc = static_cast<int>(mat_src);
        sChan[s].lightMask = light_mask;
        sChan[s].diffFn = effDiff;
        sChan[s].attnFn = static_cast<int>(attn_fn);
    };
    apply(slot);
    if (chan == GX_COLOR0A0) {
        apply(kSlotAlpha0);
    } else if (chan == GX_COLOR1A1) {
        apply(kSlotAlpha1);
    }
    PL_LOG_TRACE("gx", "GXSetChanCtrl(chan %d enable %d mask 0x%x diff %d attn %d)", slot,
                 static_cast<int>(enable), static_cast<unsigned>(light_mask),
                 static_cast<int>(diff_fn), static_cast<int>(attn_fn));
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    using namespace Platform::CompatGx;
    int pair = -1;
    switch (chan) {
    case GX_COLOR0:
    case GX_ALPHA0:
    case GX_COLOR0A0:
        pair = 0;
        break;
    case GX_COLOR1:
    case GX_ALPHA1:
    case GX_COLOR1A1:
        pair = 1;
        break;
    default:
        return;
    }
    const bool full = (chan == GX_COLOR0A0 || chan == GX_COLOR1A1);
    const bool alpha = (chan == GX_ALPHA0 || chan == GX_ALPHA1);
    if (full || !alpha) {
        sAmbColor[pair][0] = amb_color.r;
        sAmbColor[pair][1] = amb_color.g;
        sAmbColor[pair][2] = amb_color.b;
    }
    if (full || alpha) {
        sAmbColor[pair][3] = amb_color.a;
    }
    PL_LOG_TRACE("gx", "GXSetChanAmbColor(chan %d)", static_cast<int>(chan));
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    using namespace Platform::CompatGx;
    int pair = -1;
    switch (chan) {
    case GX_COLOR0:
    case GX_ALPHA0:
    case GX_COLOR0A0:
        pair = 0;
        break;
    case GX_COLOR1:
    case GX_ALPHA1:
    case GX_COLOR1A1:
        pair = 1;
        break;
    default:
        return;
    }
    const bool full = (chan == GX_COLOR0A0 || chan == GX_COLOR1A1);
    const bool alpha = (chan == GX_ALPHA0 || chan == GX_ALPHA1);
    if (full || !alpha) {
        sMatColor[pair][0] = mat_color.r;
        sMatColor[pair][1] = mat_color.g;
        sMatColor[pair][2] = mat_color.b;
    }
    if (full || alpha) {
        sMatColor[pair][3] = mat_color.a;
    }
    PL_LOG_TRACE("gx", "GXSetChanMatColor(chan %d)", static_cast<int>(chan));
}

void GXLoadLightObjImm(const GXLightObj* lt_obj, GXLightID light) {
    using namespace Platform::CompatGx;
    if (!lt_obj) {
        return;
    }
    // The SDK converts a GXLightID bit to its index (0..7). GXLightID is
    // GX_LIGHT0=1<<0 .. GX_LIGHT7=1<<7.
    unsigned idx = 0;
    unsigned mask = static_cast<unsigned>(light);
    if (mask != 0) {
        while ((mask & 1u) == 0) {
            mask >>= 1;
            ++idx;
        }
    }
    idx &= 7;
    const GXLightObjInt* obj = reinterpret_cast<const GXLightObjInt*>(lt_obj);
    LightParams& L = sLights[idx];
    // obj->Color holds the GXColor packed as a u32; the struct fields were
    // written with byte-0 = R (little-endian host: A<<24|B<<16|G<<8|R).
    const u32 c = obj->Color;
    L.color[0] = static_cast<std::uint8_t>(c & 0xFF);
    L.color[1] = static_cast<std::uint8_t>((c >> 8) & 0xFF);
    L.color[2] = static_cast<std::uint8_t>((c >> 16) & 0xFF);
    L.color[3] = static_cast<std::uint8_t>((c >> 24) & 0xFF);
    std::memcpy(L.a, obj->a, sizeof(L.a));
    std::memcpy(L.k, obj->k, sizeof(L.k));
    std::memcpy(L.pos, obj->lpos, sizeof(L.pos));
    std::memcpy(L.dir, obj->ldir, sizeof(L.dir));
    PL_LOG_TRACE("gx", "GXLoadLightObjImm(%u)", idx);
}

void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id) {
    using namespace Platform::CompatGx;
    if (id >= kMaxMtx || !mtx) {
        PL_LOG_WARN("gx", "GXLoadPosMtxImm: invalid id %u", id);
        return;
    }
    std::memcpy(sPosMtx[id], mtx, sizeof(sPosMtx[id]));
    PL_LOG_TRACE("gx", "GXLoadPosMtxImm(%u)", id);
}

void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id) {
    using namespace Platform::CompatGx;
    if (id >= kMaxMtx || !mtx) {
        PL_LOG_WARN("gx", "GXLoadNrmMtxImm: invalid id %u", id);
        return;
    }
    // The normal matrix is the 3x3 upper-left part of the 3x4 matrix. The
    // mirror is indexed by the *normal* slot (the SDK writes XF regs
    // 0x400+3*id, which is 0x400+9*slot when id = 3*slot as J3D always does;
    // GXSetCurrentMtx/MATINDEX_A select the slot as pos/3). Storing at
    // id/3 keeps every accessor consistent for the game's usage (id is
    // always a multiple of 3, i.e. a GX_PNMTX value).
    const int slot = static_cast<int>(id) / 3;
    if (slot >= kMaxMtx) {
        PL_LOG_WARN("gx", "GXLoadNrmMtxImm: invalid id %u (slot %d)", id, slot);
        return;
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            sNrmMtx[slot][r * 3 + c] = mtx[r][c];
        }
    }
    PL_LOG_TRACE("gx", "GXLoadNrmMtxImm(%u) -> nrm slot %d", id, slot);
}

void GXSetCurrentMtx(u32 id) {
    using namespace Platform::CompatGx;
    sCurPosMtx = static_cast<int>(id & 0x1F);
    sCurNrmMtx = sCurPosMtx / 3;  // GX_PNMTX ids step by 3 (pos) / 1 (nrm)
    PL_LOG_TRACE("gx", "GXSetCurrentMtx(%u) -> pos %d nrm %d", id, sCurPosMtx, sCurNrmMtx);
}

}  // extern "C"
