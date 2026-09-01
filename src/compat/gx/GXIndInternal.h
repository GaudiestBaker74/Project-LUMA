#pragma once
// =============================================================================
// compat/gx — indirect (bump) texturing stages (M5.7b) internal interface.
//
// GXInd.cpp mirrors the six GX indirect APIs (GXSetNumIndStages,
// GXSetIndTexOrder, GXSetIndTexCoordScale, GXSetIndTexMtx, GXSetTevIndirect,
// GXSetTevIndWarp / GXSetTevDirect) and packs the state into the TEV fragment
// UBO so the shader can warp the per-stage texcoord. The mirror is fed by
// both the API path and the display-list interpreter (BP registers: TEVIND
// 0x10+stage, IREF 0x24, IndTexScale 0x25/0x26, MTXA/B/C 0x06/0x07/0x08;
// GEN_MODE's num-ind-stages field routes through GXTev's dlApplyGenMode).
//
// Warp semantics (float, lockstep with gx_tev_frag.frag; Dolphin's integer
// fixpoint pipeline converted to UV space — see the shader header comment):
//   per indirect stage i (GX_INDTEXSTAGE0..3):
//     indUv  = vUV[baseCoord] / (1 << IndTexCoordScale)     // sample coord
//     itex   = texture(indMap, indUv)                       // 0..1
//     comp   = fmt-quantized rgb (8: raw; 5/4/3: 5/4/3 bits)
//     v      = comp*2 - 1 on the components covered by the bias
//              (GX_ITB_NONE keeps 0..1; SU/ST/STU center the listed ones)
//   per TEV stage with an indirect op:
//     offsetUv = (M·[v.s, v.t, 1]) * 128 / (2^scaleExp) / texDim(stage map)
//     wrapped  = base texcoord with GX_ITW_* wrapping (OFF = as-is, 0 = 0,
//                powers of two = repeat every N texels)
//     tevcoord = addPrev ? tevcoord + wrapped + offsetUv : wrapped + offsetUv
// The 128/2^scaleExp factor lives in indMtx[i].w so the shader only
// multiplies. Bump alpha (GX_ITBA_*) and the S/T matrix variants
// (GX_ITM_S0..T2) are stored but not consumed yet — see docs/gx.md §M5.7b.
// =============================================================================

#include <cstdint>

namespace Platform::CompatGx {

inline constexpr int kIndMaxStages = 4;
inline constexpr int kIndMaxMtx = 3;

// Per-indirect-stage mirror (one per GX_INDTEXSTAGE0..3). Field values are
// the raw GX enums (GXIndTexFormat, GXIndTexBiasSel, ...) so the packer can
// reproduce the exact BP register image.
struct IndStageState {
    std::int32_t texCoord = 0;  // GXTexCoordID (base coord)
    std::int32_t texMap = 0;    // GXTexMapID (indirect map)
    std::int32_t scaleS = 0;    // GXIndTexScale (1,2,4,...,128)
    std::int32_t scaleT = 0;
    std::int32_t fmt = 0;       // GX_ITF_8/5/4/3
    std::int32_t bias = 0;      // GX_ITB_NONE/S/T/ST/U/SU/TU/STU
    std::int32_t mtxSel = 0;    // GX_ITM_0/1/2 (indirect), S/T variants
    std::int32_t wrapS = 0;     // GX_ITW_OFF/256/128/64/32/16/0
    std::int32_t wrapT = 0;
    std::int32_t addPrev = 0;   // GXBool
    std::int32_t utcLod = 0;    // GXBool (stored, unused in the warp)
    std::int32_t alphaSel = 0;  // GX_ITBA_OFF/S/T/U (stored, TODO bump alpha)
};

// One 2x3 indirect matrix (GX_ITM_0..2): rows m[0]/m[1] as 3 floats each,
// exactly the `offset[2][3]` argument of GXSetIndTexMtx (the s9.2 packing is
// undone). `scaleExp` is the s8 exponent of GXSetIndTexMtx.
struct IndMtxState {
    float m[2][3] = {};
    std::int32_t scaleExp = 0;
};

// --- mirror accessors (GXInd.cpp) ---------------------------------------------

// Resets the indirect mirror to the post-GXInit defaults (0 stages, scales 1,
// GXSetTevDirect on all stages, identity matrices).
void resetIndState();

// Number of active indirect stages (GEN_MODE num-ind-stages field).
int indNumStages();

// API setters (called from the extern "C" GXSet* below and mirror the state).
void indSetNumStages(std::int32_t n);
void indSetOrder(std::int32_t stage, std::int32_t texCoord, std::int32_t texMap);
void indSetCoordScale(std::int32_t stage, std::int32_t scaleS, std::int32_t scaleT);
void indSetMtx(std::int32_t mtxId, const float offset[2][3], std::int32_t scaleExp);
void indSetTevIndirect(std::int32_t tevStage, std::int32_t indStage, std::int32_t fmt,
                       std::int32_t bias, std::int32_t mtxSel, std::int32_t wrapS,
                       std::int32_t wrapT, std::int32_t addPrev, std::int32_t utcLod,
                       std::int32_t alphaSel);

// --- display-list hooks (raw BP register values) ------------------------------
// TEVIND (RID 0x10+stage): the GXSetTevIndirect BP image.
void dlTevIndirect(std::int32_t stage, std::uint32_t value);
// IREF (RID 0x24): texMap/texCoord per ind stage.
void dlIref(std::uint32_t value);
// IndTexScale0/1 (RIDs 0x25/0x26): 4 scale fields (SS0/TS0/SS1/TS1).
void dlIndTexScale(std::uint32_t value, bool high);
// MTXA/B/C (RIDs 0x06 + 3*id): two 11-bit s9.2 fields + 2 exponent bits each.
void dlIndMtxReg(std::int32_t rid, std::uint32_t value);

// --- UBO packing --------------------------------------------------------------
// Writes the indirect fields of the TEV UBO (see GXTevInternal.h): indParams
// (per ind stage: coord|map|scales|fmt/bias/mtxSel/wraps/addPrev packed),
// indMtx (3 matrices x 2 rows; row = (m0,m1,m2) and .w = 128/2^scaleExp) and
// per-stage tevind packs (stage[i].reserved[0]).
void packIndTevUbo(struct TevUboData& out);

// --- CPU reference (unit tests, lockstep with the shader) ---------------------
// Pure warp evaluator: given the base texcoord uv (0..1), the indirect texel
// components comp (0..1, per format), the 2x3 matrix rows, the exponent and
// the direct-map texel size, returns the warped texcoord (0..1). Mirrors the
// shader's offsetUv = (M·[v,1]) * 128 / 2^scaleExp / texDim.
void evalIndirectWarp(const float uv[2], const float comp[3], const float mtx[2][3],
                      std::int32_t scaleExp, const float texDim[2], std::int32_t bias,
                      std::int32_t wrapS, std::int32_t wrapT, float outUv[2]);

// Debug accessor for tests.
struct GxIndDebugState {
    int numStages = 0;
    int texCoord[kIndMaxStages] = {};
    int texMap[kIndMaxStages] = {};
    int scaleS[kIndMaxStages] = {};
    int scaleT[kIndMaxStages] = {};
    int fmt[kIndMaxStages] = {};
    int bias[kIndMaxStages] = {};
    int mtxSel[kIndMaxStages] = {};
    int wrapS[kIndMaxStages] = {};
    int wrapT[kIndMaxStages] = {};
    int addPrev[kIndMaxStages] = {};
    float mtx[kIndMaxMtx][2][3] = {};
    int mtxScaleExp[kIndMaxMtx] = {};
};
void getIndDebugState(GxIndDebugState& out);

}  // namespace Platform::CompatGx
