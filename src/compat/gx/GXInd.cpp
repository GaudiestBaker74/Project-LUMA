// =============================================================================
// compat/gx — indirect (bump) texturing stages (M5.7b).
//
// Mirror + UBO packing for the six GX indirect APIs. The GXSet* wrappers
// (extern "C") decode exactly like the vendored GXBump.c and update the
// semantic mirror; the display-list interpreter routes the BP registers
// (TEVIND 0x10+stage, IREF 0x24, IndTexScale 0x25/0x26, MTXA/B/C 0x06..0x08,
// GEN_MODE num-ind-stages) through the dl* hooks. packIndTevUbo fills the
// M5.7b fields of the TEV fragment UBO; the warp itself runs in
// gx_tev_frag.frag with the same float formula implemented here by
// evalIndirectWarp (kept in lockstep, verified by the unit tests).
//
// Float warp semantics (Dolphin's integer fixpoint converted to UV space):
//   * sample: indUv = vUV[baseCoord] / (1 << IndTexCoordScale) — the indirect
//     map is sampled with the base coord scaled DOWN (the console's
//     "fixpoint_uv >> scale").
//   * quantize the texel to the format (ITF_8 raw, ITF_5/4/3 top 5/4/3 bits)
//     and center the components covered by GX_ITB_* (0..1 -> -1..1).
//   * offset in texels = (M·[s,t,1]) * 128 / 2^scaleExp (the console's
//     s9.2 x 8-bit fixpoint product >> 3 >> scale); normalized to UV by the
//     direct map's texel size, then added to the wrapped stage texcoord.
// Known deviations (documented in docs/gx.md §M5.7b): the GX_ITM_S0..T2
// matrix variants and the bump alpha (GX_ITBA_*) are stored but not used by
// the warp; the power-of-two wraps (GX_ITW_256..16) are approximated with
// fract instead of integer masking. The game's water materials (OceanBowl /
// OceanRingDrawer) use ITW_OFF + ITM_0, which is exact.
// =============================================================================

#include "compat/gx/GXIndInternal.h"
#include "compat/gx/GXTevInternal.h"  // TevUboData
#include "compat/gx/GXCompat.h"

#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXBump.h>

#include "platform/Log/Log.h"

#include <cmath>
#include <cstring>

namespace Platform::CompatGx {
namespace {

int sNumIndStages = 0;
IndStageState sIndStage[kIndMaxStages];
IndMtxState sIndMtx[kIndMaxMtx];

// Per-TEV-stage indirect config (the GXSetTevIndirect image). Only stages
// that J3D/API configure are populated; the rest default to "direct" (no
// warp): indStage 0 with GX_ITM_OFF (matrix off => no indirect at all).
struct StageIndirect {
    std::int32_t indStage = 0;
    std::int32_t fmt = 0;       // GX_ITF_8
    std::int32_t bias = 0;      // GX_ITB_NONE
    std::int32_t mtxSel = 0;    // GX_ITM_OFF
    std::int32_t wrapS = 0;     // GX_ITW_OFF
    std::int32_t wrapT = 0;
    std::int32_t addPrev = 0;
    std::int32_t utcLod = 0;
    std::int32_t alphaSel = 0;  // GX_ITBA_OFF
};
StageIndirect sStageInd[kTevMaxStages];

// GX_ITM_0..2 -> matrix slot (1..3 -> 0..2; everything else is stored as-is
// for the debug accessor).
int mtxSlot(std::int32_t sel) {
    return (sel >= GX_ITM_0 && sel <= GX_ITM_2) ? sel - GX_ITM_0 : -1;
}

}  // namespace

// --- mirror ----------------------------------------------------------------

void resetIndState() {
    sNumIndStages = 0;
    for (auto& s : sIndStage) {
        s = IndStageState{};
    }
    for (auto& s : sStageInd) {
        s = StageIndirect{};
    }
    for (auto& m : sIndMtx) {
        m = IndMtxState{};
    }
}

void indSetNumStages(std::int32_t n) {
    sNumIndStages = (n >= 0 && n <= kIndMaxStages) ? n : 0;
}

int indNumStages() {
    return sNumIndStages;
}

void indSetOrder(std::int32_t stage, std::int32_t texCoord, std::int32_t texMap) {
    if (stage < 0 || stage >= kIndMaxStages) {
        return;
    }
    sIndStage[stage].texCoord = texCoord;
    sIndStage[stage].texMap = texMap;
}

void indSetCoordScale(std::int32_t stage, std::int32_t scaleS, std::int32_t scaleT) {
    if (stage < 0 || stage >= kIndMaxStages) {
        return;
    }
    sIndStage[stage].scaleS = scaleS;
    sIndStage[stage].scaleT = scaleT;
}

void indSetMtx(std::int32_t mtxId, const float offset[2][3], std::int32_t scaleExp) {
    const int slot = mtxSlot(mtxId);
    if (slot < 0 || slot >= kIndMaxMtx || !offset) {
        return;
    }
    std::memcpy(sIndMtx[slot].m, offset, sizeof(sIndMtx[slot].m));
    sIndMtx[slot].scaleExp = scaleExp;
}

void indSetTevIndirect(std::int32_t tevStage, std::int32_t indStage, std::int32_t fmt,
                       std::int32_t bias, std::int32_t mtxSel, std::int32_t wrapS,
                       std::int32_t wrapT, std::int32_t addPrev, std::int32_t utcLod,
                       std::int32_t alphaSel) {
    if (tevStage < 0 || tevStage >= kTevMaxStages) {
        return;
    }
    StageIndirect& s = sStageInd[tevStage];
    s.indStage = (indStage >= 0 && indStage < kIndMaxStages) ? indStage : 0;
    s.fmt = fmt;
    s.bias = bias;
    s.mtxSel = mtxSel;
    s.wrapS = wrapS;
    s.wrapT = wrapT;
    s.addPrev = addPrev;
    s.utcLod = utcLod;
    s.alphaSel = alphaSel;
}

// --- display-list hooks ------------------------------------------------------

void dlTevIndirect(std::int32_t stage, std::uint32_t value) {
    if (stage < 0 || stage >= kTevMaxStages) {
        return;
    }
    // BP_CMD fields (bp_reg.h): BT 0, FMT 2, BIAS 4, BS 7, M 9, SW 13, TW 16,
    // LB 19, FB 20 — the image GXSetTevIndirect writes.
    StageIndirect& s = sStageInd[stage];
    s.indStage = static_cast<std::int32_t>(value & 0x3);
    s.fmt = static_cast<std::int32_t>((value >> 2) & 0x3);
    s.bias = static_cast<std::int32_t>((value >> 4) & 0x7);
    s.alphaSel = static_cast<std::int32_t>((value >> 7) & 0x3);
    s.mtxSel = static_cast<std::int32_t>((value >> 9) & 0xF);
    s.wrapS = static_cast<std::int32_t>((value >> 13) & 0x7);
    s.wrapT = static_cast<std::int32_t>((value >> 16) & 0x7);
    s.utcLod = static_cast<std::int32_t>((value >> 19) & 0x1);
    s.addPrev = static_cast<std::int32_t>((value >> 20) & 0x1);
    PL_LOG_TRACE("gx", "DL TEVIND stage %d: bt %d fmt %d bias %d mtx %d sw %d tw %d fb %d",
                 stage, s.indStage, s.fmt, s.bias, s.mtxSel, s.wrapS, s.wrapT, s.addPrev);
}

void dlIref(std::uint32_t value) {
    // RAS1_IREF (ras_reg.h): BI0..BI3 at 0,4,8,12; BC0..BC3 at 16,20,24,28.
    for (int i = 0; i < kIndMaxStages; ++i) {
        sIndStage[i].texMap = static_cast<std::int32_t>((value >> (4 * i)) & 0x7);
        sIndStage[i].texCoord = static_cast<std::int32_t>((value >> (16 + 4 * i)) & 0x7);
    }
}

void dlIndTexScale(std::uint32_t value, bool high) {
    // RAS1_SS (ras_reg.h): SS0 0, TS0 4, SS1 8, TS1 12. Register 0x25 holds
    // stages 0/1, 0x26 holds stages 2/3.
    const int base = high ? 2 : 0;
    sIndStage[base + 0].scaleS = static_cast<std::int32_t>(value & 0x7);
    sIndStage[base + 0].scaleT = static_cast<std::int32_t>((value >> 4) & 0x7);
    sIndStage[base + 1].scaleS = static_cast<std::int32_t>((value >> 8) & 0x7);
    sIndStage[base + 1].scaleT = static_cast<std::int32_t>((value >> 12) & 0x7);
}

void dlIndMtxReg(std::int32_t rid, std::uint32_t value) {
    // BP_MTXA/B/C (bp_reg.h): each register carries one COLUMN of the 2x3
    // matrix as two 11-bit s9.2 fields at shifts 0 and 11 (MA/MB = column 0,
    // MC/MD = column 1, ME/MF = column 2 — see GXBump.c) plus a 2-bit
    // exponent field S at shift 22. The three registers of a matrix carry
    // the 6 exponent bits (scale_exp + 0x11).
    const int id = (rid - 0x06) / 3;
    const int col = (rid - 0x06) % 3;
    if (id < 0 || id >= kIndMaxMtx) {
        return;
    }
    const int expBits = static_cast<int>((value >> 22) & 0x3);
    IndMtxState& m = sIndMtx[id];
    const auto raw11 = [](std::uint32_t v, int shift) {
        const int raw = static_cast<int>((v >> shift) & 0x7FF);
        return static_cast<float>((raw & 0x400) ? raw - 0x800 : raw) / 1024.0f;
    };
    m.m[0][col] = raw11(value, 0);
    m.m[1][col] = raw11(value, 11);
    m.scaleExp = static_cast<std::int32_t>((m.scaleExp & ~(0x3 << (2 * col))) |
                                           (expBits << (2 * col)));
    // The full 6-bit exponent is assembled after all three registers arrive;
    // GXSetIndTexMtx stores scale_exp + 0x11 (see GXBump.c). Decode it only
    // when the third register of the matrix has been written.
    if (col == 2) {
        const int v6 = m.scaleExp & 0x3F;
        m.scaleExp = v6 - 0x11;
    }
}

// --- UBO packing -------------------------------------------------------------

void packIndTevUbo(TevUboData& out) {
    for (int i = 0; i < 4; ++i) {
        out.indParams[i][0] = out.indParams[i][1] = out.indParams[i][2] =
            out.indParams[i][3] = 0;
        if (i < kIndMaxStages) {
            const IndStageState& s = sIndStage[i];
            out.indParams[i][0] = s.texCoord | (s.texMap << 8) | (s.scaleS << 16) |
                                  (s.scaleT << 24);
        }
    }
    for (int m = 0; m < kIndMaxMtx; ++m) {
        const IndMtxState& sm = sIndMtx[m];
        for (int r = 0; r < 2; ++r) {
            out.indMtx[m][r][0] = sm.m[r][0];
            out.indMtx[m][r][1] = sm.m[r][1];
            out.indMtx[m][r][2] = sm.m[r][2];
            // Texel-space calibration: 128 / 2^scaleExp (see the header).
            out.indMtx[m][r][3] = 128.0f * std::pow(2.0f, static_cast<float>(sm.scaleExp));
        }
    }
    for (int i = 0; i < kTevMaxStages; ++i) {
        const StageIndirect& s = sStageInd[i];
        // stage[i].reserved[0] = tevind pack (shader reads it as stage[i][3].x).
        out.stage[i].reserved[0] =
            (s.indStage & 0xF) | ((s.fmt & 0xF) << 4) | ((s.bias & 0xF) << 8) |
            ((s.mtxSel & 0xF) << 12) | ((s.wrapS & 0x7) << 16) |
            ((s.wrapT & 0x7) << 20) | ((s.addPrev & 0x1) << 24);
    }
}

// --- CPU reference warp (lockstep with gx_tev_frag.frag) ----------------------

void evalIndirectWarp(const float uv[2], const float comp[3], const float mtx[2][3],
                      std::int32_t scaleExp, const float texDim[2], std::int32_t bias,
                      std::int32_t wrapS, std::int32_t wrapT, float outUv[2]) {
    // Center the components covered by the bias (GX_ITB_S/T/U bit flags).
    float v[3] = {comp[0], comp[1], comp[2]};
    if (bias & 1) v[0] = v[0] * 2.0f - 1.0f;
    if (bias & 2) v[1] = v[1] * 2.0f - 1.0f;
    if (bias & 4) v[2] = v[2] * 2.0f - 1.0f;

    // Offset in texels = (M·[s,t,1]) * 128 / 2^scaleExp, normalized to UV.
    float offsetUv[2] = {
        (mtx[0][0] * v[0] + mtx[0][1] * v[1] + mtx[0][2]) *
            (128.0f * std::pow(2.0f, static_cast<float>(scaleExp))),
        (mtx[1][0] * v[0] + mtx[1][1] * v[1] + mtx[1][2]) *
            (128.0f * std::pow(2.0f, static_cast<float>(scaleExp))),
    };
    if (texDim[0] > 0.0f) offsetUv[0] /= texDim[0];
    if (texDim[1] > 0.0f) offsetUv[1] /= texDim[1];

    // Wrap the base texcoord (GX_ITW_*): OFF = as-is, 0 = zero, 256..16 =
    // repeat every N texels (mod, approximated in UV).
    float wrapped[2] = {uv[0], uv[1]};
    if (wrapS == 6) {
        wrapped[0] = 0.0f;
    } else if (wrapS >= 1 && wrapS <= 5) {
        const float n = static_cast<float>(1 << (9 - wrapS));
        if (texDim[0] > 0.0f) {
            const float period = n / texDim[0];
            wrapped[0] = (uv[0] - std::floor(uv[0] / period) * period);
        }
    }
    if (wrapT == 6) {
        wrapped[1] = 0.0f;
    } else if (wrapT >= 1 && wrapT <= 5) {
        const float n = static_cast<float>(1 << (9 - wrapT));
        if (texDim[1] > 0.0f) {
            const float period = n / texDim[1];
            wrapped[1] = (uv[1] - std::floor(uv[1] / period) * period);
        }
    }
    outUv[0] = wrapped[0] + offsetUv[0];
    outUv[1] = wrapped[1] + offsetUv[1];
}

// --- debug -------------------------------------------------------------------

void getIndDebugState(GxIndDebugState& out) {
    out = GxIndDebugState{};
    out.numStages = sNumIndStages;
    for (int i = 0; i < kIndMaxStages; ++i) {
        out.texCoord[i] = sIndStage[i].texCoord;
        out.texMap[i] = sIndStage[i].texMap;
        out.scaleS[i] = sIndStage[i].scaleS;
        out.scaleT[i] = sIndStage[i].scaleT;
    }
    for (int i = 0; i < kTevMaxStages; ++i) {
        const StageIndirect& s = sStageInd[i];
        if (i < kIndMaxStages) {
            out.fmt[i] = s.fmt;
            out.bias[i] = s.bias;
            out.mtxSel[i] = s.mtxSel;
            out.wrapS[i] = s.wrapS;
            out.wrapT[i] = s.wrapT;
            out.addPrev[i] = s.addPrev;
        }
    }
    for (int m = 0; m < kIndMaxMtx; ++m) {
        std::memcpy(out.mtx[m], sIndMtx[m].m, sizeof(sIndMtx[m].m));
        out.mtxScaleExp[m] = sIndMtx[m].scaleExp;
    }
}

}  // namespace Platform::CompatGx

// --- GX API implementations ---------------------------------------------------

extern "C" {

void GXSetNumIndStages(u8 nIndStages) {
    Platform::CompatGx::indSetNumStages(nIndStages);
    PL_LOG_TRACE("gx", "GXSetNumIndStages(%u)", static_cast<unsigned>(nIndStages));
}

void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    if (tex_map == GX_TEXMAP_NULL) {
        tex_map = GX_TEXMAP0;
    }
    if (tex_coord == GX_TEXCOORD_NULL) {
        tex_coord = GX_TEXCOORD0;
    }
    Platform::CompatGx::indSetOrder(ind_stage, tex_coord, tex_map);
    PL_LOG_TRACE("gx", "GXSetIndTexOrder(stage %d, coord %d, map %d)",
                 static_cast<int>(ind_stage), static_cast<int>(tex_coord),
                 static_cast<int>(tex_map));
}

void GXSetIndTexCoordScale(GXIndTexStageID ind_stage, GXIndTexScale scale_s,
                           GXIndTexScale scale_t) {
    Platform::CompatGx::indSetCoordScale(ind_stage, scale_s, scale_t);
    PL_LOG_TRACE("gx", "GXSetIndTexCoordScale(stage %d, s %d, t %d)",
                 static_cast<int>(ind_stage), static_cast<int>(scale_s),
                 static_cast<int>(scale_t));
}

void GXSetIndTexMtx(GXIndTexMtxID mtx_id, const f32 offset[2][3], s8 scale_exp) {
    Platform::CompatGx::indSetMtx(mtx_id, offset, scale_exp);
    PL_LOG_TRACE("gx", "GXSetIndTexMtx(id %d, exp %d)",
                 static_cast<int>(mtx_id), static_cast<int>(scale_exp));
}

void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,
                      GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s,
                      GXIndTexWrap wrap_t, GXBool add_prev, GXBool utc_lod,
                      GXIndTexAlphaSel alpha_sel) {
    Platform::CompatGx::indSetTevIndirect(tev_stage, ind_stage, format, bias_sel, matrix_sel,
                                          wrap_s, wrap_t, add_prev != GX_FALSE,
                                          utc_lod != GX_FALSE, alpha_sel);
    PL_LOG_TRACE("gx", "GXSetTevIndirect(stage %d, ind %d, fmt %d, bias %d, mtx %d)",
                 static_cast<int>(tev_stage), static_cast<int>(ind_stage),
                 static_cast<int>(format), static_cast<int>(bias_sel),
                 static_cast<int>(matrix_sel));
}

void GXSetTevDirect(GXTevStageID tev_stage) {
    // The GXBump.c default: indirect stage 0, ITF_8, no bias, matrix off.
    Platform::CompatGx::indSetTevIndirect(tev_stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE,
                                          GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, 0, 0,
                                          GX_ITBA_OFF);
}

void GXSetTevIndWarp(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXBool signed_offset,
                     GXBool replace_mode, GXIndTexMtxID matrix_sel) {
    GXIndTexWrap wrap = (replace_mode != GX_FALSE) ? GX_ITW_0 : GX_ITW_OFF;
    Platform::CompatGx::indSetTevIndirect(tev_stage, ind_stage, GX_ITF_8,
                                          (signed_offset != GX_FALSE) ? GX_ITB_STU
                                                                      : GX_ITB_NONE,
                                          matrix_sel, wrap, wrap, 0, 0, GX_ITBA_OFF);
}

}  // extern "C"
