// =============================================================================
// compat/gx — TEV (texture environment unit) + fragment pixel-engine state
// mirror, CPU reference evaluators (M5.4/M5.5).
//
// Model: the GX configuration calls mirror the console register state
// (per-stage color/alpha combiners, TEV order, K constants, swap tables, the
// C0..C2 registers, the number of stages, the alpha compare and the fog
// registers). At draw time GXCompat::flushDraw packs the mirror into a std140
// UBO (buildTevUbo) uploaded to the TEV fragment shader, which evaluates up
// to 16 chained stages per pixel, then applies the pixel-engine output stage
// (alpha test, "alpha == 1" blend quirk, fog).
//
// The per-stage formula is the GameCube/Wii TEV formula as implemented by
// Dolphin's PixelShaderGen (verified against the vendored SDK register
// semantics):
//
//   out = ((D + bias) << sl) ± (((((A<<8) + (B-A)·(C+(C>>7))) << sl) + r) >> 8)
//   out >>= sr
//     sl/sr from scale (2/4 shift the whole lerp+D, DIVIDE_2 shifts the total)
//     r = 128 (ADD) / 127 (SUB), skipped for DIVIDE_2
//     clamp: [0,255] or [-1024,1023]
//
// Comparison ops (GX_TEV_COMP_*, op >= 8) select C when a comparison of A vs B
// (mode from (op>>1)&3, GT/EQ from op&1) holds; the alpha combiner's R8/GR16/
// BGR24 modes compare the *color* combiner's A/B args (Dolphin semantics).
//
// The fog math (GXSetFog) mirrors the vendored GXPixel.c exactly: perspective
// fog packs A/B/C with B normalized into the B_MAG (u0.24) + B_SHF registers,
// orthographic fog stores a/c linearly; the shader applies Dolphin's WriteFog
// formulas (persp: ze = A·2^24/(B_MAG − (Zs >> B_SHF)); ortho: ze = A·Zs/2^24,
// fog = clamp(ze − C, 0, 1), then the fsel density functions and the
// 8-bit ifog blend toward the fog color).
//
// evalTevChain()/evalPixelEngine() are the CPU references of the shader,
// kept in lockstep with tools/shaders/gx_tev_frag.frag; the unit tests pin
// them, and the offscreen Vulkan test verifies the GPU path against them.
// =============================================================================

#include "compat/gx/GXTevInternal.h"
#include "compat/gx/GXIndInternal.h"

#include "compat/gx/GXCompat.h"

#include "platform/Log/Log.h"

#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXTev.h>
// BP register bit layouts (the display-list decoder reads raw registers).
#include <private/tev_reg.h>
#include <private/ras_reg.h>

#include <cmath>
#include <cstring>

namespace Platform::CompatGx {

// --- GXInit defaults (from the vendored GXInit.c, lines ~225-475) -------------
// tevc/teva registers are zeroed (only the register ids are set): color env =
// a=b=c=d=CPREV, ADD, bias ZERO, scale 1, clamp FALSE, dest TEVPREV; alpha
// env = a=b=c=d=APREV, swap SWAP0/SWAP0. Stage 0 then gets GXSetTevOp(REPLACE)
// (clamp TRUE). TevOrder: stages 0..7 = (TEXCOORDn, TEXMAPn, COLOR0A0),
// stages 8..15 = (NULL, NULL, NULL -> zero channel). KCSEL 1_4 / KASEL 1,
// swap SWAP0/SWAP0, swap tables SWAP0=RGBA SWAP1=RRRA SWAP2=GGGA SWAP3=BBBA,
// numTevStages 1, numTexGens 1 (GXInit calls GXSetNumTexGens(1)).
// Pixel engine: alpha compare ALWAYS/0/AND/ALWAYS/0; fog NONE with the
// GXSetFog(GX_FOG_NONE, 0, 1, 0.1, 1, black) parameters (through the
// perspective branch: A = 0.1/0.9/4, C = 0, B_MAG = 0.5555...·8388638,
// B_SHF = 2); fog range adj disabled.

namespace {

struct TevEnv {
    int cA = GX_CC_CPREV, cB = GX_CC_CPREV, cC = GX_CC_CPREV, cD = GX_CC_CPREV;
    int aA = GX_CA_APREV, aB = GX_CA_APREV, aC = GX_CA_APREV, aD = GX_CA_APREV;
    int cOp = GX_TEV_ADD, cBias = GX_TB_ZERO, cScale = GX_CS_SCALE_1;
    int cClamp = 0, cDest = GX_TEVPREV;
    int aOp = GX_TEV_ADD, aBias = GX_TB_ZERO, aScale = GX_CS_SCALE_1;
    int aClamp = 0, aDest = GX_TEVPREV;
    int rasSel = GX_TEV_SWAP0, texSel = GX_TEV_SWAP0;
};

struct TevOrder {
    int texmap = 0;     // clamped GXTexMapID (valid when texEnable)
    int texcoord = 0;   // GXTexCoordID
    int colorChan = 0;  // c2r-mapped: 0=col0, 1=col1, 5/6=bump, 7=zero
    int texEnable = 0;  // TE bit (map != NULL && !DISABLE)
};

// Stage color/alpha preset args for GXSetTevOp (from the vendored GXTev.c
// TEVCOpTableST0/ST1 and TEVAOpTableST0/ST1).
struct TevOpPreset {
    int cA, cB, cC, cD;
    int aA, aB, aC, aD;
};
// Stage 0 (RASC/RASA sources).
constexpr TevOpPreset kOpPresetST0[5] = {
    // MODULATE
    {GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO},
    // DECAL
    {GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA},
    // BLEND
    {GX_CC_RASC, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO},
    // REPLACE
    {GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA},
    // PASSCLR
    {GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA},
};
// Stages >= 1 (CPREV/APREV sources).
constexpr TevOpPreset kOpPresetST1[5] = {
    // MODULATE
    {GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO},
    // DECAL
    {GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV},
    // BLEND
    {GX_CC_CPREV, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO},
    // REPLACE
    {GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA},
    // PASSCLR
    {GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV},
};

// Pixel-engine state that packs into the fragment UBO (M5.5). Written by
// GXSetFog / GXSetAlphaCompare, read by buildTevUbo.
struct PeState {
    float fogA = 0.0f;   // decoded TEV_FOG_PARAM_0 (persp A or ortho a)
    float fogC = 0.0f;   // decoded TEV_FOG_PARAM_3 (persp C or ortho c)
    int fogBMagnitude = 0;  // B_MAG (u0.24) — only written by persp fog
    int fogBShift = 0;      // B_SHF (exponent+1)
    int fogFsel = 0;        // type & 7
    int fogProj = 0;        // (type >> 3) & 1
    std::int32_t fogColor[4] = {};
    std::int32_t alphaComp0 = GX_ALWAYS, alphaRef0 = 0;
    std::int32_t alphaLogic = GX_AOP_AND;
    std::int32_t alphaComp1 = GX_ALWAYS, alphaRef1 = 0;
};

int sNumTevStages = 1;
int sNumTexGens = 1;
TevEnv sEnv[kTevMaxStages];
TevOrder sOrder[kTevMaxStages];
int sKColorSel[kTevMaxStages];
int sKAlphaSel[kTevMaxStages];
std::int32_t sKonst[4][4] = {};
std::int32_t sTevReg[3][4] = {};
std::int32_t sTevPrev[4] = {};
std::int32_t sSwapTable[4][4] = {};
PeState sPe;

// GXSetTevOrder's channel-to-raster mapping (vendored GXTev.c c2r[]):
// GX_COLOR0=0, GX_COLOR1=1, GX_ALPHA0=2, GX_ALPHA1=3, GX_COLOR0A0=4,
// GX_COLOR1A1=5, GX_COLOR_ZERO=6, GX_ALPHA_BUMP=7, GX_ALPHA_BUMPN=8.
// Values >= 4 in the mapped space are 7(zero)/5/6(bump) — the shader reads
// 0=col0, 1=col1, anything else = zero (bump needs ind stages, M5.7).
constexpr int kC2R[9] = {0, 1, 0, 1, 0, 1, 7, 5, 6};

// GX_TEXMAP_NULL / GX_TEX_DISABLE mask (GXEnum.h): 255 / 256.
constexpr int kTexMapNull = 255;
constexpr int kTexDisable = 256;

// Reinterprets a float as its IEEE-754 bit pattern (the fog registers are
// built from the raw float32 bits of a/c).
uint32_t floatToBits(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

// --- CPU reference evaluator helpers (mirror of the shader) -------------------

int konstChannel(int sel, int chan, const std::int32_t konst[4][4]) {
    // Fixed fractions: 1, 7/8, 6/8, 5/8, 4/8, 3/8, 2/8, 1/8 -> 0..255.
    static const int kFracs[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    if (sel >= 0 && sel <= 7) {
        return kFracs[sel];
    }
    // sel 8..11 (0x08-0x0B): invalid selects, treated as 0 (Dolphin). No GX
    // enum produces them; kept for lockstep with the shader.
    if (sel >= 12 && sel <= 15) return konst[sel - 12][chan];
    if (sel >= 16 && sel <= 19) return konst[sel - 16][0];
    if (sel >= 20 && sel <= 23) return konst[sel - 20][1];
    if (sel >= 24 && sel <= 27) return konst[sel - 24][2];
    if (sel >= 28 && sel <= 31) return konst[sel - 28][3];
    return 0;
}
int konstAlpha(int sel, const std::int32_t konst[4][4]) {
    // K0..K3 (12..15) are color-only selects; alpha reads 0 (Dolphin).
    return (sel >= 12 && sel <= 15) ? 0 : konstChannel(sel, 0, konst);
}

void fetchColorArg(int arg, const int prev[4], const int c0[4], const int c1[4],
                   const int c2[4], const int textemp[4], const int rastemp[4],
                   const int konsttemp[4], int out[3]) {
    switch (arg) {
        case GX_CC_CPREV: out[0] = prev[0]; out[1] = prev[1]; out[2] = prev[2]; break;
        case GX_CC_APREV: out[0] = out[1] = out[2] = prev[3]; break;
        case GX_CC_C0:    out[0] = c0[0]; out[1] = c0[1]; out[2] = c0[2]; break;
        case GX_CC_A0:    out[0] = out[1] = out[2] = c0[3]; break;
        case GX_CC_C1:    out[0] = c1[0]; out[1] = c1[1]; out[2] = c1[2]; break;
        case GX_CC_A1:    out[0] = out[1] = out[2] = c1[3]; break;
        case GX_CC_C2:    out[0] = c2[0]; out[1] = c2[1]; out[2] = c2[2]; break;
        case GX_CC_A2:    out[0] = out[1] = out[2] = c2[3]; break;
        case GX_CC_TEXC:  out[0] = textemp[0]; out[1] = textemp[1]; out[2] = textemp[2]; break;
        case GX_CC_TEXA:  out[0] = out[1] = out[2] = textemp[3]; break;
        case GX_CC_RASC:  out[0] = rastemp[0]; out[1] = rastemp[1]; out[2] = rastemp[2]; break;
        case GX_CC_RASA:  out[0] = out[1] = out[2] = rastemp[3]; break;
        case GX_CC_ONE:   out[0] = out[1] = out[2] = 255; break;
        case GX_CC_HALF:  out[0] = out[1] = out[2] = 128; break;
        case GX_CC_KONST: out[0] = konsttemp[0]; out[1] = konsttemp[1]; out[2] = konsttemp[2]; break;
        default:          out[0] = out[1] = out[2] = 0; break;  // ZERO
    }
}
int fetchAlphaArg(int arg, const int prev[4], const int c0[4], const int c1[4],
                  const int c2[4], const int textemp[4], const int rastemp[4],
                  const int konsttemp[4]) {
    switch (arg) {
        case GX_CA_APREV: return prev[3];
        case GX_CA_A0:    return c0[3];
        case GX_CA_A1:    return c1[3];
        case GX_CA_A2:    return c2[3];
        case GX_CA_TEXA:  return textemp[3];
        case GX_CA_RASA:  return rastemp[3];
        case GX_CA_KONST: return konsttemp[3];
        default:          return 0;  // ZERO
    }
}

// Regular combine (Dolphin formula, see file header).
int tevCombine(int a, int b, int c, int d, int op, int bias, int scale) {
    const int sl = (scale == GX_CS_SCALE_2) ? 1 : (scale == GX_CS_SCALE_4) ? 2 : 0;
    const int biasV = (bias == GX_TB_ADDHALF) ? 128 : (bias == GX_TB_SUBHALF) ? -128 : 0;
    int lerp = (a << 8) + (b - a) * (c + (c >> 7));  // c>>7: arithmetic shift
    if (scale != GX_CS_DIVIDE_2) {
        lerp = (lerp << sl) + ((op == GX_TEV_ADD) ? 128 : 127);
    }
    lerp >>= 8;
    const int db = (d + biasV) << sl;
    int out = (op == GX_TEV_ADD) ? (db + lerp) : (db - lerp);
    if (scale == GX_CS_DIVIDE_2) {
        out >>= 1;
    }
    return out;
}

int clampReg(int v, int clamp) {
    return clamp ? (v < 0 ? 0 : (v > 255 ? 255 : v))
                 : (v < -1024 ? -1024 : (v > 1023 ? 1023 : v));
}

void evalStage(const TevUboData& ubo, const TevChainInputs& in, int s, int prev[4],
               int c0[4], int c1[4], int c2[4]) {
    const std::int32_t* cenv = ubo.stage[s].colorEnv;
    const std::int32_t* aenv = ubo.stage[s].alphaEnv;
    const std::int32_t* opPack = ubo.stage[s].opParams;
    const int colorOpPack = opPack[0];
    const int alphaOpPack = opPack[1];
    const int orderPack = opPack[2];
    const int kselPack = opPack[3];

    const int texmap = orderPack & 0xFF;
    int texcoord = (orderPack >> 8) & 0xFF;
    const int colorChan = (orderPack >> 16) & 0xFF;
    const bool texEnable = ((orderPack >> 24) & 1) != 0;
    // Quirk (Dolphin): a stage texcoord >= the number of texgens falls back to
    // texcoord 0 (the coordinate "does not exist").
    const int texGenCount = ubo.header[1];
    if (texcoord >= texGenCount) {
        texcoord = 0;
    }

    // Raster color source (0=col0, 1=col1; bump/zero for others, M5.7).
    int ras[4] = {0, 0, 0, 0};
    if (colorChan == 0) {
        std::memcpy(ras, in.ras[0], sizeof(ras));
    } else if (colorChan == 1) {
        std::memcpy(ras, in.ras[1], sizeof(ras));
    }

    // Texture: enabled + texgens>0 -> sample; no texgens -> black; else white.
    int texel[4] = {255, 255, 255, 255};
    if (texEnable && texGenCount > 0) {
        std::memcpy(texel, in.texel[texmap], sizeof(texel));
    } else if (texGenCount == 0) {
        std::memset(texel, 0, sizeof(texel));
    }

    // Swap tables (rasSel/texSel from the alpha op pack).
    const int* rasSwap = ubo.swapTables[(alphaOpPack >> 12) & 3];
    const int* texSwap = ubo.swapTables[(alphaOpPack >> 14) & 3];
    int rastemp[4] = {ras[rasSwap[0]], ras[rasSwap[1]], ras[rasSwap[2]], ras[rasSwap[3]]};
    int textemp[4] = {texel[texSwap[0]], texel[texSwap[1]], texel[texSwap[2]], texel[texSwap[3]]};

    int konsttemp[4] = {konstChannel(kselPack & 0xFF, 0, ubo.konst),
                        konstChannel(kselPack & 0xFF, 1, ubo.konst),
                        konstChannel(kselPack & 0xFF, 2, ubo.konst),
                        konstAlpha((kselPack >> 8) & 0xFF, ubo.konst)};

    // --- color combine ------------------------------------------------------
    const int cOp = colorOpPack & 0x1F;
    const int cBias = (colorOpPack >> 5) & 3;
    const int cScale = (colorOpPack >> 7) & 3;
    const int cClamp = (colorOpPack >> 9) & 1;
    const int cDest = (colorOpPack >> 10) & 3;

    int cA[3], cB[3], cC[3], cD[3];
    fetchColorArg(cenv[0], prev, c0, c1, c2, textemp, rastemp, konsttemp, cA);
    fetchColorArg(cenv[1], prev, c0, c1, c2, textemp, rastemp, konsttemp, cB);
    fetchColorArg(cenv[2], prev, c0, c1, c2, textemp, rastemp, konsttemp, cC);
    fetchColorArg(cenv[3], prev, c0, c1, c2, textemp, rastemp, konsttemp, cD);

    int cout[3];
    if (cOp >= 8) {
        const int cmpMode = (cOp >> 1) & 3;
        const bool eq = (cOp & 1) == 1;
        int csel[3] = {0, 0, 0};
        if (cmpMode == 0) {  // R8
            const bool hit = eq ? (cA[0] == cB[0]) : (cA[0] > cB[0]);
            if (hit) { csel[0] = cC[0]; csel[1] = cC[1]; csel[2] = cC[2]; }
        } else if (cmpMode == 1) {  // GR16 (weights 1,256,0)
            const int da = cA[0] + 256 * cA[1];
            const int db = cB[0] + 256 * cB[1];
            const bool hit = eq ? (da == db) : (da > db);
            if (hit) { csel[0] = cC[0]; csel[1] = cC[1]; csel[2] = cC[2]; }
        } else if (cmpMode == 2) {  // BGR24 (weights 1,256,65536)
            const int da = cA[0] + 256 * cA[1] + 65536 * cA[2];
            const int db = cB[0] + 256 * cB[1] + 65536 * cB[2];
            const bool hit = eq ? (da == db) : (da > db);
            if (hit) { csel[0] = cC[0]; csel[1] = cC[1]; csel[2] = cC[2]; }
        } else {  // RGB8
            for (int k = 0; k < 3; ++k) {
                const int d = cA[k] - cB[k];
                const int sgn = (d > 0) - (d < 0);
                if (eq) {
                    csel[k] = (sgn == 0) ? cC[k] : 0;
                } else {
                    csel[k] = (sgn > 0) ? cC[k] : 0;
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            cout[k] = cD[k] + csel[k];
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            cout[k] = tevCombine(cA[k], cB[k], cC[k], cD[k], cOp, cBias, cScale);
        }
    }
    for (int k = 0; k < 3; ++k) {
        cout[k] = clampReg(cout[k], cClamp);
    }
    int* destC = (cDest == 1) ? c0 : (cDest == 2) ? c1 : (cDest == 3) ? c2 : prev;
    std::memcpy(destC, cout, sizeof(cout));

    // --- alpha combine ------------------------------------------------------
    const int aOp = alphaOpPack & 0x1F;
    const int aBias = (alphaOpPack >> 5) & 3;
    const int aScale = (alphaOpPack >> 7) & 3;
    const int aClamp = (alphaOpPack >> 9) & 1;
    const int aDest = (alphaOpPack >> 10) & 3;

    const int aA = fetchAlphaArg(aenv[0], prev, c0, c1, c2, textemp, rastemp, konsttemp);
    const int aB = fetchAlphaArg(aenv[1], prev, c0, c1, c2, textemp, rastemp, konsttemp);
    const int aC = fetchAlphaArg(aenv[2], prev, c0, c1, c2, textemp, rastemp, konsttemp);
    const int aD = fetchAlphaArg(aenv[3], prev, c0, c1, c2, textemp, rastemp, konsttemp);

    int aout;
    if (aOp >= 8) {
        const int cmpMode = (aOp >> 1) & 3;
        const bool eq = (aOp & 1) == 1;
        int asel = 0;
        if (cmpMode == 0) {  // R8 — compares the COLOR combiner's A/B red
            const bool hit = eq ? (cA[0] == cB[0]) : (cA[0] > cB[0]);
            if (hit) asel = aC;
        } else if (cmpMode == 1) {  // GR16
            const int da = cA[0] + 256 * cA[1];
            const int db = cB[0] + 256 * cB[1];
            const bool hit = eq ? (da == db) : (da > db);
            if (hit) asel = aC;
        } else if (cmpMode == 2) {  // BGR24
            const int da = cA[0] + 256 * cA[1] + 65536 * cA[2];
            const int db = cB[0] + 256 * cB[1] + 65536 * cB[2];
            const bool hit = eq ? (da == db) : (da > db);
            if (hit) asel = aC;
        } else {  // A8 — compares the alpha combiner's own A/B
            const bool hit = eq ? (aA == aB) : (aA > aB);
            if (hit) asel = aC;
        }
        aout = aD + asel;
    } else {
        aout = tevCombine(aA, aB, aC, aD, aOp, aBias, aScale);
    }
    aout = clampReg(aout, aClamp);
    int* destA = (aDest == 1) ? c0 : (aDest == 2) ? c1 : (aDest == 3) ? c2 : prev;
    destA[3] = aout;
}

// --- pixel-engine output stage (mirror of the shader main) --------------------

// GXCompare semantics (GXEnum.h: NEVER=0 ... ALWAYS=7) on (alpha, ref).
bool alphaCompare(int comp, int alpha, int ref) {
    switch (comp) {
        case GX_NEVER:   return false;
        case GX_LESS:    return alpha < ref;
        case GX_EQUAL:   return alpha == ref;
        case GX_LEQUAL:  return alpha <= ref;
        case GX_GREATER: return alpha > ref;
        case GX_NEQUAL:  return alpha != ref;
        case GX_GEQUAL:  return alpha >= ref;
        default:         return true;  // GX_ALWAYS
    }
}

// Decodes the 20-bit TEV_FOG_PARAM_0 register value (A) back to float:
// mant(11) | exp(8) | sign(1) -> float32 with the low 12 mantissa bits zeroed
// (Dolphin FogParam0::FloatValue). NaN -> 0 (Dolphin FogParams::GetA).
float decodeFogParamA(uint32_t bits) {
    const uint32_t mant = (bits >> 12) & 0x7FF;
    const uint32_t expn = (bits >> 23) & 0xFF;
    if (expn == 0xFF && mant != 0) {
        return 0.0f;
    }
    const uint32_t integral = ((bits >> 31) << 31) | (expn << 23) | (mant << 12);
    float v;
    std::memcpy(&v, &integral, sizeof(v));
    return v;
}
// Same for TEV_FOG_PARAM_3 (C). NaN -> ±inf per Dolphin FogParams::GetC.
float decodeFogParamC(uint32_t bits, uint32_t aBits) {
    const uint32_t mant = (bits >> 12) & 0x7FF;
    const uint32_t expn = (bits >> 23) & 0xFF;
    if (expn == 0xFF && mant != 0) {
        const bool neg = ((aBits >> 31) & 1) == 0 && ((bits >> 31) & 1) == 0;
        return neg ? -INFINITY : INFINITY;
    }
    const uint32_t integral = ((bits >> 31) << 31) | (expn << 23) | (mant << 12);
    float v;
    std::memcpy(&v, &integral, sizeof(v));
    return v;
}

} // namespace

void evalTevChain(const TevUboData& ubo, const TevChainInputs& in, std::int32_t out[4]) {
    int prev[4], c0[4], c1[4], c2[4];
    std::memcpy(prev, ubo.prev, sizeof(prev));
    std::memcpy(c0, ubo.tevReg[0], sizeof(c0));
    std::memcpy(c1, ubo.tevReg[1], sizeof(c1));
    std::memcpy(c2, ubo.tevReg[2], sizeof(c2));

    int numStages = ubo.header[0];
    if (numStages < 0) numStages = 0;
    if (numStages > kTevMaxStages) numStages = kTevMaxStages;
    for (int s = 0; s < numStages; ++s) {
        evalStage(ubo, in, s, prev, c0, c1, c2);
    }
    // The last stage's result goes to the screen regardless of its dest
    // register (Dolphin quirk).
    if (numStages > 0) {
        const TevUboData::Stage& last = ubo.stage[numStages - 1];
        const int cDest = (last.opParams[0] >> 10) & 3;
        const int aDest = (last.opParams[1] >> 10) & 3;
        if (cDest != GX_TEVPREV) {
            const int* src = (cDest == 1) ? c0 : (cDest == 2) ? c1 : c2;
            std::memcpy(prev, src, 3 * sizeof(int));
        }
        if (aDest != GX_TEVPREV) {
            const int* src = (aDest == 1) ? c0 : (aDest == 2) ? c1 : c2;
            prev[3] = src[3];
        }
    }
    std::memcpy(out, prev, sizeof(prev));
}

void evalPixelEngine(const TevUboData& ubo, const TevChainInputs& in, int zCoord,
                     bool& discarded, std::int32_t out[4]) {
    int prev[4];
    evalTevChain(ubo, in, prev);

    // 8-bit output stage: the TEV output is cut to 8 bits per component
    // (Dolphin: "prev = frag_output.main & 255").
    for (int i = 0; i < 4; ++i) {
        prev[i] &= 255;
    }

    // --- alpha test (GXSetAlphaCompare) --------------------------------------
    const int pack = ubo.alphaCmp[0];
    const int comp0 = pack & 7, ref0 = (pack >> 3) & 0xFF;
    const int logic = (pack >> 11) & 3;
    const int comp1 = (pack >> 13) & 7, ref1 = (pack >> 16) & 0xFF;
    const bool pass0 = alphaCompare(comp0, prev[3], ref0);
    const bool pass1 = alphaCompare(comp1, prev[3], ref1);
    bool pass;
    switch (logic) {  // GX_AOP_AND=0, OR=1, XOR=2, XNOR=3
        case GX_AOP_AND:  pass = pass0 && pass1; break;
        case GX_AOP_OR:   pass = pass0 || pass1; break;
        case GX_AOP_XOR:  pass = pass0 != pass1; break;
        default:          pass = pass0 == pass1; break;  // GX_AOP_XNOR
    }
    discarded = !pass;
    if (discarded) {
        return;
    }

    // Hardware quirk (Dolphin): "an alpha of 1 can pass an alpha test, but
    // doesn't do anything in blending" — zero the alpha so fixed-function
    // blending leaves the framebuffer unchanged.
    if (prev[3] == 1) {
        prev[3] = 0;
    }

    // --- fog (GXSetFog, Dolphin WriteFog) ------------------------------------
    const int fsel = ubo.fogReg[2];
    if (fsel != 0) {
        const float fogA = ubo.fogAB[0];
        const float fogC = ubo.fogAB[1];
        const int bMagnitude = ubo.fogReg[0];
        const int bShift = ubo.fogReg[1];
        const int proj = ubo.fogReg[3];

        float ze;
        if (proj == 0) {  // perspective: ze = A·2^24/(B_MAG − (Zs >> B_SHF))
            const int zs = zCoord >> bShift;
            ze = (fogA * 16777216.0f) / static_cast<float>(bMagnitude - zs);
        } else {          // orthographic: ze = A·Zs/2^24
            ze = fogA * static_cast<float>(zCoord) / 16777216.0f;
        }
        float fog = ze - fogC;
        if (fog < 0.0f) fog = 0.0f;
        if (fog > 1.0f) fog = 1.0f;
        switch (fsel) {
            case 4:  fog = 1.0f - std::exp2(-8.0f * fog); break;           // EXP
            case 5:  fog = 1.0f - std::exp2(-8.0f * fog * fog); break;     // EXP2
            case 6:  fog = std::exp2(-8.0f * (1.0f - fog)); break;         // REVEXP
            case 7:  fog = 1.0f - fog; fog = std::exp2(-8.0f * fog * fog); break;  // REVEXP2
            default: break;  // 2 = linear (fog = clamp(ze − C, 0, 1))
        }
        const int ifog = static_cast<int>(std::round(fog * 256.0f));
        for (int k = 0; k < 3; ++k) {
            prev[k] = (prev[k] * (256 - ifog) + ubo.fogColor[k] * ifog) >> 8;
        }
    }

    std::memcpy(out, prev, sizeof(prev));
}

void resetTevState() {
    sNumTevStages = 1;
    sNumTexGens = 1;
    Platform::CompatGx::resetIndState();
    for (int i = 0; i < kTevMaxStages; ++i) {
        sEnv[i] = TevEnv();
        sOrder[i] = TevOrder();
        sKColorSel[i] = GX_TEV_KCSEL_1_4;
        sKAlphaSel[i] = GX_TEV_KASEL_1;
    }
    // TevOrder: stages 0..7 bind (TEXCOORDn, TEXMAPn, COLOR0A0); 8..15 are
    // NULL/NULL/NULL (color zero). Mirrors the GXInit GXSetTevOrder calls.
    for (int i = 0; i < 8; ++i) {
        sOrder[i].texmap = i;
        sOrder[i].texcoord = i;
        sOrder[i].colorChan = 0;  // COLOR0A0 -> c2r[4] = 0
        sOrder[i].texEnable = 1;
    }
    for (int i = 8; i < kTevMaxStages; ++i) {
        sOrder[i].texEnable = 0;
        sOrder[i].colorChan = 7;  // NULL -> zero
    }
    // Stage 0: GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE) after the register reset.
    {
        const TevOpPreset& p = kOpPresetST0[GX_REPLACE];
        TevEnv& e = sEnv[0];
        e.cA = p.cA; e.cB = p.cB; e.cC = p.cC; e.cD = p.cD;
        e.aA = p.aA; e.aB = p.aB; e.aC = p.aC; e.aD = p.aD;
        e.cClamp = 1;
        e.aClamp = 1;
    }
    std::memset(sKonst, 0, sizeof(sKonst));
    std::memset(sTevReg, 0, sizeof(sTevReg));
    std::memset(sTevPrev, 0, sizeof(sTevPrev));
    // Swap tables: SWAP0=RGBA, SWAP1=RRRA, SWAP2=GGGA, SWAP3=BBBA.
    const std::int32_t kSwapInit[4][4] = {
        {0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3}};
    std::memcpy(sSwapTable, kSwapInit, sizeof(sSwapTable));
    // Pixel-engine defaults: alpha compare ALWAYS/0/AND/ALWAYS/0 (GXInit);
    // fog = the GXInit GXSetFog(GX_FOG_NONE, 0, 1, 0.1, 1, black) parameters
    // (perspective branch: A = 0.1/0.9/2^2, C = 0, B_MAG = 0.5555...·8388638,
    // B_SHF = 2), fsel 0 (off).
    PeState p{};
    p.alphaComp0 = GX_ALWAYS;
    p.alphaComp1 = GX_ALWAYS;
    p.alphaLogic = GX_AOP_AND;
    // a = (farz−nearz)/(endz−startz) = 0.9/1.0 (persp branch), then the
    // register packing truncates; C = 0; B normalized from 1.1111...
    const float A = (1.0f * 0.1f) / ((1.0f - 0.1f) * (1.0f - 0.0f));
    const float B = 1.0f / (1.0f - 0.1f);
    const float C = 0.0f / (1.0f - 0.0f);
    float bMant = B;
    int bExpn = 0;
    while (bMant > 1.0f) { bMant /= 2.0f; ++bExpn; }
    while ((bMant > 0.0f) && (bMant < 0.5f)) { bMant *= 2.0f; --bExpn; }
    p.fogA = decodeFogParamA(floatToBits(A / std::ldexp(1.0f, bExpn + 1)));
    p.fogC = decodeFogParamC(floatToBits(C), floatToBits(A / std::ldexp(1.0f, bExpn + 1)));
    p.fogBMagnitude = static_cast<int>(bMant * 8388638.0f);
    p.fogBShift = bExpn + 1;
    p.fogFsel = 0;
    p.fogProj = 0;
    sPe = p;
}

void buildTevUbo(TevUboData& out) {
    std::memset(&out, 0, sizeof(out));
    std::memcpy(out.prev, sTevPrev, sizeof(sTevPrev));
    std::memcpy(out.tevReg, sTevReg, sizeof(sTevReg));
    std::memcpy(out.konst, sKonst, sizeof(sKonst));
    std::memcpy(out.swapTables, sSwapTable, sizeof(sSwapTable));
    out.header[0] = sNumTevStages;
    out.header[1] = sNumTexGens;
    out.header[2] = Platform::CompatGx::indNumStages();
    // M5.7b indirect stages: per-stage tevind packs + per-ind-stage params +
    // matrices. texDims is filled by GXCompat::flushDraw (it knows the
    // bound TEXMAPs' sizes, which the UBO builder here does not).
    Platform::CompatGx::packIndTevUbo(out);
    for (int i = 0; i < kTevMaxStages; ++i) {
        const TevEnv& e = sEnv[i];
        const TevOrder& o = sOrder[i];
        out.stage[i].colorEnv[0] = e.cA;
        out.stage[i].colorEnv[1] = e.cB;
        out.stage[i].colorEnv[2] = e.cC;
        out.stage[i].colorEnv[3] = e.cD;
        out.stage[i].alphaEnv[0] = e.aA;
        out.stage[i].alphaEnv[1] = e.aB;
        out.stage[i].alphaEnv[2] = e.aC;
        out.stage[i].alphaEnv[3] = e.aD;
        out.stage[i].opParams[0] = packColorOp(e.cOp, e.cBias, e.cScale, e.cClamp, e.cDest);
        out.stage[i].opParams[1] =
            packAlphaOp(e.aOp, e.aBias, e.aScale, e.aClamp, e.aDest, e.rasSel, e.texSel);
        out.stage[i].opParams[2] =
            packOrder(o.texEnable ? o.texmap : 0, o.texcoord, o.colorChan, o.texEnable);
        out.stage[i].opParams[3] = packKsel(sKColorSel[i], sKAlphaSel[i]);
    }
    // M5.5 pixel-engine extras.
    std::memcpy(out.fogColor, sPe.fogColor, sizeof(sPe.fogColor));
    out.fogReg[0] = sPe.fogBMagnitude;
    out.fogReg[1] = sPe.fogBShift;
    out.fogReg[2] = sPe.fogFsel;
    out.fogReg[3] = sPe.fogProj;
    out.fogAB[0] = sPe.fogA;
    out.fogAB[1] = sPe.fogC;
    out.alphaCmp[0] = (sPe.alphaComp0 & 7) | ((sPe.alphaRef0 & 0xFF) << 3) |
                      ((sPe.alphaLogic & 3) << 11) | ((sPe.alphaComp1 & 7) << 13) |
                      ((sPe.alphaRef1 & 0xFF) << 16);
}

int tevNumStages() {
    return sNumTevStages;
}

int tevTexGenCount() {
    return sNumTexGens;
}

void setFogState(float a, float c, std::int32_t bMagnitude, std::int32_t bShift,
                 std::int32_t fsel, std::int32_t proj, const std::uint8_t color[4]) {
    const uint32_t aBits = floatToBits(a);
    const uint32_t cBits = floatToBits(c);
    sPe.fogA = decodeFogParamA(aBits);
    sPe.fogC = decodeFogParamC(cBits, aBits);
    sPe.fogBMagnitude = bMagnitude;
    sPe.fogBShift = bShift;
    sPe.fogFsel = fsel;
    sPe.fogProj = proj;
    for (int i = 0; i < 4; ++i) {
        sPe.fogColor[i] = color[i];
    }
}

void setFogRangeAdjState(bool enable, int center, const void* table) {
    // M5.5: mirror the state only (the shader does not consume it yet).
    // TODO(PC_PORT, M5.7): implement fog range adjustment (Dolphin's
    // x_adjust = sqrt(offset² + k²)/k with the K table and the viewport
    // center) in the fragment shader.
    (void)enable;
    (void)center;
    (void)table;
    PL_LOG_TRACE("gx", "GXSetFogRangeAdj: state stored (not yet rendered)");
}

void setAlphaCompareState(std::int32_t comp0, std::int32_t ref0, std::int32_t logic,
                          std::int32_t comp1, std::int32_t ref1) {
    sPe.alphaComp0 = comp0;
    sPe.alphaRef0 = ref0;
    sPe.alphaLogic = logic;
    sPe.alphaComp1 = comp1;
    sPe.alphaRef1 = ref1;
}

void dlApplyGenMode(std::uint32_t value) {
    const int ntev = static_cast<int>((value >> 10) & 0xF) + 1;
    const int ntex = static_cast<int>(value & 0xF);
    const int nind = static_cast<int>((value >> 6) & 0x3);  // NBMP
    if (ntev >= 1 && ntev <= kTevMaxStages) {
        sNumTevStages = ntev;
    }
    sNumTexGens = (ntex <= 8) ? ntex : 8;
    Platform::CompatGx::indSetNumStages(nind);
}

// Reconstructs the GX_TEV_* op from the register fields: for compare ops
// (bias == GX_MAX_TEVBIAS) the SHIFT field is the compare mode (0=R8,
// 1=GR16, 2=BGR24, 3=RGB8) and the SUB bit is GT/EQ; for regular ops the
// SUB bit is ADD/SUB and SHIFT is the scale.
int tevOpFromReg(int sub, int bias, int shift) {
    if (bias == GX_MAX_TEVBIAS) {
        return 8 + shift * 2 + sub;  // GX_TEV_COMP_R8_GT..RGB8_EQ
    }
    return sub;  // GX_TEV_ADD / GX_TEV_SUB
}

void dlApplyBpTev(std::uint8_t rid, std::uint32_t value) {
    // --- TEVC/TEVA stage combiners (0xC0 + 2*stage, 0xC1 + 2*stage) ----------
    if (rid >= 0xC0 && rid <= 0xDF) {
        const int stage = (rid - 0xC0) / 2;
        const bool isAlpha = ((rid - 0xC0) & 1) != 0;
        if (stage < 0 || stage >= kTevMaxStages) {
            return;
        }
        TevEnv& e = sEnv[stage];
        const int sub = isAlpha ? TEV_ALPHA_ENV_GET_SUB(value) : TEV_COLOR_ENV_GET_SUB(value);
        const int bias = isAlpha ? TEV_ALPHA_ENV_GET_BIAS(value) : TEV_COLOR_ENV_GET_BIAS(value);
        const int shift = isAlpha ? TEV_ALPHA_ENV_GET_SHIFT(value)
                                  : TEV_COLOR_ENV_GET_SHIFT(value);
        const int clamp = isAlpha ? TEV_ALPHA_ENV_GET_CLAMP(value)
                                  : TEV_COLOR_ENV_GET_CLAMP(value);
        const int dest = isAlpha ? TEV_ALPHA_ENV_GET_DEST(value) : TEV_COLOR_ENV_GET_DEST(value);
        const int op = tevOpFromReg(sub, bias, shift);
        if (isAlpha) {
            e.aA = TEV_ALPHA_ENV_GET_SELA(value);
            e.aB = TEV_ALPHA_ENV_GET_SELB(value);
            e.aC = TEV_ALPHA_ENV_GET_SELC(value);
            e.aD = TEV_ALPHA_ENV_GET_SELD(value);
            e.aOp = op;
            if (op <= GX_TEV_SUB) {
                e.aBias = bias;
                e.aScale = shift;
            } else {
                e.aScale = shift;
                e.aBias = GX_MAX_TEVBIAS;
            }
            e.aClamp = clamp;
            e.aDest = dest;
            e.rasSel = TEV_ALPHA_ENV_GET_MODE(value);
            e.texSel = TEV_ALPHA_ENV_GET_SWAP(value);
        } else {
            e.cA = TEV_COLOR_ENV_GET_SELA(value);
            e.cB = TEV_COLOR_ENV_GET_SELB(value);
            e.cC = TEV_COLOR_ENV_GET_SELC(value);
            e.cD = TEV_COLOR_ENV_GET_SELD(value);
            e.cOp = op;
            if (op <= GX_TEV_SUB) {
                e.cBias = bias;
                e.cScale = shift;
            } else {
                e.cScale = shift;
                e.cBias = GX_MAX_TEVBIAS;
            }
            e.cClamp = clamp;
            e.cDest = dest;
        }
        return;
    }

    // --- TEV color/constant registers (0xE0 + 2*id, bit 23 = type) -----------
    if (rid >= 0xE0 && rid <= 0xE7) {
        const int id = (rid - 0xE0) / 2;
        const bool isH = ((rid - 0xE0) & 1) != 0;
        const bool isKonst = (value & 0x00800000) != 0;
        if (id < 0 || id > 3) {
            return;
        }
        if (isKonst) {
            // Constant registers (GXSetTevKColor): 8-bit R/G/B/A.
            std::int32_t* k = sKonst[id];
            if (isH) {
                k[2] = (value >> 0) & 0xFF;      // B
                k[1] = (value >> 8) & 0xFF;      // G
            } else {
                k[0] = (value >> 0) & 0xFF;      // R
                k[3] = (value >> 12) & 0xFF;     // A
            }
        } else {
            // Color registers (GXSetTevColor/S10): signed 11-bit R/G/B/A.
            // REGISTERL packs R (bits 0-10) + A (bits 12-22); REGISTERH packs
            // B (bits 0-10) + G (bits 12-22); each is an 11-bit two's
            // complement value.
            const auto s11 = [](int v) {
                v &= 0x7FF;
                return (v & 0x400) ? v - 0x800 : v;
            };
            std::int32_t* r = (id == 0) ? sTevPrev : sTevReg[id - 1];
            if (isH) {
                r[2] = s11((value >> 0) & 0x7FF);   // B
                r[1] = s11((value >> 12) & 0x7FF);  // G
            } else {
                r[0] = s11((value >> 0) & 0x7FF);   // R
                r[3] = s11((value >> 12) & 0x7FF);  // A
            }
        }
        return;
    }

    // --- fog registers (0xEE..0xF2) -------------------------------------------
    if (rid == 0xEE) {  // TEV_FOG_PARAM_0: A
        const uint32_t bits = ((value >> 19) & 1) << 31 | ((value >> 11) & 0xFF) << 23 |
                              ((value >> 0) & 0x7FF) << 12;
        sPe.fogA = decodeFogParamA(bits);
        return;
    }
    if (rid == 0xEF) {  // B_MAG (u0.24)
        sPe.fogBMagnitude = static_cast<std::int32_t>(value & 0xFFFFFF);
        return;
    }
    if (rid == 0xF0) {  // B_SHF
        sPe.fogBShift = static_cast<std::int32_t>(value & 0x1F);
        return;
    }
    if (rid == 0xF1) {  // TEV_FOG_PARAM_3: C + proj + fsel
        const uint32_t bits = ((value >> 19) & 1) << 31 | ((value >> 11) & 0xFF) << 23 |
                              ((value >> 0) & 0x7FF) << 12;
        sPe.fogC = decodeFogParamC(bits, floatToBits(sPe.fogA));
        sPe.fogProj = static_cast<std::int32_t>((value >> 20) & 1);
        sPe.fogFsel = static_cast<std::int32_t>((value >> 21) & 7);
        return;
    }
    if (rid == 0xF2) {  // fog color RGB
        sPe.fogColor[0] = static_cast<std::int32_t>((value >> 0) & 0xFF);
        sPe.fogColor[1] = static_cast<std::int32_t>((value >> 8) & 0xFF);
        sPe.fogColor[2] = static_cast<std::int32_t>((value >> 16) & 0xFF);
        sPe.fogColor[3] = 0;
        return;
    }

    // --- alpha compare (0xF3) ---------------------------------------------------
    if (rid == 0xF3) {
        setAlphaCompareState(TEV_ALPHAFUNC_GET_OP0(value), TEV_ALPHAFUNC_GET_A0(value),
                             TEV_ALPHAFUNC_GET_LOGIC(value), TEV_ALPHAFUNC_GET_OP1(value),
                             TEV_ALPHAFUNC_GET_A1(value));
        return;
    }

    // --- TEV K-selects + swap tables (0xF6..0xFD) ------------------------------
    // Each KSEL register k (RID 0xF6+k) holds the color/alpha K-selects for
    // stages 2k and 2k+1 (KCSEL0/KCSEL1, KASEL0/KASEL1) plus two 2-bit swap
    // table entries (XRB/XGA). The swap tables are split across register
    // pairs: register 2t holds table t's R/G channels, register 2t+1 its
    // B/A channels (mirrors GXSetTevSwapModeTable / GXSetTevKColorSel).
    if (rid >= 0xF6 && rid <= 0xFD) {
        const int k = rid - 0xF6;
        const int stageBase = 2 * k;
        const int table = k / 2;
        const bool isSwapBA = (k & 1) != 0;
        // The 4-bit swap-table R/G (or B/A on odd regs) share the register.
        const int xrb = static_cast<int>(value & 0x3);
        const int xga = static_cast<int>((value >> 2) & 0x3);
        if (!isSwapBA) {
            sSwapTable[table][0] = xrb;
            sSwapTable[table][1] = xga;
        } else {
            sSwapTable[table][2] = xrb;
            sSwapTable[table][3] = xga;
        }
        sKColorSel[stageBase] = TEV_KSEL_GET_KCSEL0(value);
        sKAlphaSel[stageBase] = TEV_KSEL_GET_KASEL0(value);
        sKColorSel[stageBase + 1] = TEV_KSEL_GET_KCSEL1(value);
        sKAlphaSel[stageBase + 1] = TEV_KSEL_GET_KASEL1(value);
        return;
    }

    // --- rasterizer TREF (0x28..0x2F): per-stage-pair texmap/coord/chan -------
    if (rid >= 0x28 && rid <= 0x2F) {
        const int pair = rid - 0x28;
        for (int half = 0; half < 2; ++half) {
            const int stage = pair * 2 + half;
            const int ti = half ? RAS1_TREF_GET_TI1(value) : RAS1_TREF_GET_TI0(value);
            const int tc = half ? RAS1_TREF_GET_TC1(value) : RAS1_TREF_GET_TC0(value);
            const int te = half ? RAS1_TREF_GET_TE1(value) : RAS1_TREF_GET_TE0(value);
            const int cc = half ? RAS1_TREF_GET_CC1(value) : RAS1_TREF_GET_CC0(value);
            TevOrder& o = sOrder[stage];
            o.texEnable = te;
            int tmap = ti & ~kTexDisable;
            o.texmap = (te && tmap < GX_MAX_TEXMAP) ? tmap : 0;
            o.texcoord = (tc >= 8) ? 0 : tc;
            o.colorChan = cc;
        }
        return;
    }

    // --- M5.7b: indirect stages ----------------------------------------------
    // TEVIND (0x10 + stage), IREF (0x24), IndTexScale0/1 (0x25/0x26) and the
    // indirect matrices MTXA/B/C (0x06 + 3*id). The values arrive as the
    // 24-bit BP payloads the vendored GXBump.c builds (see GXInd.cpp).
    if (rid >= 0x06 && rid <= 0x08) {
        Platform::CompatGx::dlIndMtxReg(rid, value);
        return;
    }
    if (rid >= 0x10 && rid <= 0x1F) {
        Platform::CompatGx::dlTevIndirect(rid - 0x10, value);
        return;
    }
    if (rid == 0x24) {
        Platform::CompatGx::dlIref(value);
        return;
    }
    if (rid == 0x25 || rid == 0x26) {
        Platform::CompatGx::dlIndTexScale(value, rid == 0x26);
        return;
    }

    PL_LOG_TRACE("gx", "dlApplyBpTev: RID 0x%02X not handled (mirror unchanged)", rid);
}

} // namespace Platform::CompatGx

// --- GX API implementations ---------------------------------------------------

extern "C" {

using namespace Platform::CompatGx;

void GXSetNumTevStages(u8 nStages) {
    if (nStages < 1 || nStages > kTevMaxStages) {
        PL_LOG_WARN("gx", "GXSetNumTevStages(%u) out of range — ignored",
                    static_cast<unsigned>(nStages));
        return;
    }
    sNumTevStages = nStages;
    PL_LOG_TRACE("gx", "GXSetNumTevStages(%u)", static_cast<unsigned>(nStages));
}

void GXSetNumTexGens(u8 nTexGens) {
    if (nTexGens > 8) {
        nTexGens = 8;
    }
    sNumTexGens = nTexGens;
    PL_LOG_TRACE("gx", "GXSetNumTexGens(%u)", static_cast<unsigned>(nTexGens));
}

void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    if (id < GX_TEVSTAGE0 || id > GX_TEVSTAGE15) {
        PL_LOG_WARN("gx", "GXSetTevOp: invalid stage %d", static_cast<int>(id));
        return;
    }
    if (mode < GX_MODULATE || mode > GX_PASSCLR) {
        PL_LOG_WARN("gx", "GXSetTevOp: invalid mode %d", static_cast<int>(mode));
        return;
    }
    PL_LOG_TRACE("gx", "GXSetTevOp(stage %d, mode %d)", static_cast<int>(id),
                 static_cast<int>(mode));
    // Stage 0 uses the RASC/RASA presets; stages >= 1 the CPREV/APREV ones
    // (vendored GXTev.c TEVCOpTableST0/ST1). All presets: ADD, bias ZERO,
    // scale 1, clamp TRUE, dest TEVPREV.
    const TevOpPreset& p = (id == GX_TEVSTAGE0) ? kOpPresetST0[mode] : kOpPresetST1[mode];
    TevEnv& e = sEnv[id];
    e.cA = p.cA; e.cB = p.cB; e.cC = p.cC; e.cD = p.cD;
    e.aA = p.aA; e.aB = p.aB; e.aC = p.aC; e.aD = p.aD;
    e.cOp = GX_TEV_ADD; e.cBias = GX_TB_ZERO; e.cScale = GX_CS_SCALE_1;
    e.cClamp = 1; e.cDest = GX_TEVPREV;
    e.aOp = GX_TEV_ADD; e.aBias = GX_TB_ZERO; e.aScale = GX_CS_SCALE_1;
    e.aClamp = 1; e.aDest = GX_TEVPREV;
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                     GXTevColorArg d) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    TevEnv& e = sEnv[stage];
    e.cA = a; e.cB = b; e.cC = c; e.cD = d;
    PL_LOG_TRACE("gx", "GXSetTevColorIn(stage %d, %d %d %d %d)", static_cast<int>(stage),
                 a, b, c, d);
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                     GXTevAlphaArg d) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    TevEnv& e = sEnv[stage];
    e.aA = a; e.aB = b; e.aC = c; e.aD = d;
    PL_LOG_TRACE("gx", "GXSetTevAlphaIn(stage %d, %d %d %d %d)", static_cast<int>(stage),
                 a, b, c, d);
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out_reg) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    TevEnv& e = sEnv[stage];
    e.cOp = op;
    if (op <= GX_TEV_SUB) {
        e.cBias = bias;
        e.cScale = scale;
    } else {
        // Comparison ops: shift bits are the compare mode, bias forced to max.
        e.cScale = ((op >> 1) & 3);
        e.cBias = GX_MAX_TEVBIAS;
    }
    e.cClamp = (clamp != GX_FALSE);
    e.cDest = out_reg;
    PL_LOG_TRACE("gx", "GXSetTevColorOp(stage %d, op %d)", static_cast<int>(stage), op);
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out_reg) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    TevEnv& e = sEnv[stage];
    e.aOp = op;
    if (op <= GX_TEV_SUB) {
        e.aBias = bias;
        e.aScale = scale;
    } else {
        e.aScale = ((op >> 1) & 3);
        e.aBias = GX_MAX_TEVBIAS;
    }
    e.aClamp = (clamp != GX_FALSE);
    e.aDest = out_reg;
    PL_LOG_TRACE("gx", "GXSetTevAlphaOp(stage %d, op %d)", static_cast<int>(stage), op);
}

// Sign-extends a 10-bit value to int (the TEV registers are signed 10-bit).
static int s10(int v) {
    v &= 0x3FF;
    return (v & 0x200) ? v - 0x400 : v;
}

void GXSetTevColor(GXTevRegID id, GXColor color) {
    if (id < GX_TEVPREV || id > GX_TEVREG2) {
        return;
    }
    // TEVPREV is the initial value of the shader's prev accumulator; C0..C2
    // are the persistent registers. Mirrors the hardware registers.
    std::int32_t* r = (id == GX_TEVPREV) ? sTevPrev : sTevReg[id - 1];
    r[0] = color.r;
    r[1] = color.g;
    r[2] = color.b;
    r[3] = color.a;
    PL_LOG_TRACE("gx", "GXSetTevColor(%d, %u %u %u %u)", static_cast<int>(id), color.r,
                 color.g, color.b, color.a);
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    if (id < GX_TEVPREV || id > GX_TEVREG2) {
        return;
    }
    std::int32_t* r = (id == GX_TEVPREV) ? sTevPrev : sTevReg[id - 1];
    r[0] = s10(color.r);
    r[1] = s10(color.g);
    r[2] = s10(color.b);
    r[3] = s10(color.a);
    PL_LOG_TRACE("gx", "GXSetTevColorS10(%d)", static_cast<int>(id));
}

void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    if (id < GX_KCOLOR0 || id > GX_KCOLOR3) {
        return;
    }
    std::int32_t* k = sKonst[id];
    k[0] = color.r;
    k[1] = color.g;
    k[2] = color.b;
    k[3] = color.a;
    PL_LOG_TRACE("gx", "GXSetTevKColor(%d)", static_cast<int>(id));
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    sKColorSel[stage] = sel;
    PL_LOG_TRACE("gx", "GXSetTevKColorSel(stage %d, sel %d)", static_cast<int>(stage), sel);
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    sKAlphaSel[stage] = sel;
    PL_LOG_TRACE("gx", "GXSetTevKAlphaSel(stage %d, sel %d)", static_cast<int>(stage), sel);
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    sEnv[stage].rasSel = ras_sel;
    sEnv[stage].texSel = tex_sel;
    PL_LOG_TRACE("gx", "GXSetTevSwapMode(stage %d, ras %d tex %d)", static_cast<int>(stage),
                 ras_sel, tex_sel);
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                           GXTevColorChan blue, GXTevColorChan alpha) {
    if (table < GX_TEV_SWAP0 || table > GX_TEV_SWAP3) {
        return;
    }
    sSwapTable[table][0] = red;
    sSwapTable[table][1] = green;
    sSwapTable[table][2] = blue;
    sSwapTable[table][3] = alpha;
    PL_LOG_TRACE("gx", "GXSetTevSwapModeTable(%d, %d %d %d %d)", static_cast<int>(table),
                 red, green, blue, alpha);
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    if (stage < GX_TEVSTAGE0 || stage > GX_TEVSTAGE15) {
        return;
    }
    PL_LOG_TRACE("gx", "GXSetTevOrder(stage %d, coord %d, map %d, color %d)",
                 static_cast<int>(stage), static_cast<int>(coord), static_cast<int>(map),
                 static_cast<int>(color));
    TevOrder& o = sOrder[stage];
    o.texmap = map;
    // Mirror the vendored GXTev.c: the texmap id is masked against GX_TEX_DISABLE
    // and clamped to GX_TEXMAP0 when >= GX_MAX_TEXMAP.
    int tmap = map & ~kTexDisable;
    if (tmap >= GX_MAX_TEXMAP) {
        tmap = GX_TEXMAP0;
    }
    o.texEnable = (map != kTexMapNull && !(map & kTexDisable));
    if (o.texEnable) {
        o.texmap = tmap;
    }
    o.texcoord = (coord >= GX_MAX_TEXCOORD) ? GX_TEXCOORD0 : coord;
    o.colorChan = (color == GX_COLOR_NULL) ? 7
                                           : (color >= 0 && color < 9) ? kC2R[color] : 7;
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    PL_LOG_TRACE("gx", "GXSetAlphaCompare(%d, %u, %d, %d, %u)", static_cast<int>(comp0),
                 static_cast<unsigned>(ref0), static_cast<int>(op), static_cast<int>(comp1),
                 static_cast<unsigned>(ref1));
    setAlphaCompareState(comp0, ref0, op, comp1, ref1);
}

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    // Mirror of the vendored GXPixel.c GXSetFog (register computation only —
    // there is no BP FIFO on PC). proj = (type >> 3) & 1: 1 = orthographic,
    // 0 = perspective (GX_FOG_ORTHO_* have bit 3 set, GX_FOG_PERSP_* not).
    const int fsel = static_cast<int>(type & 0x07);
    const int proj = static_cast<int>((type >> 3) & 0x01);

    float a = 0.0f, c = 0.0f;
    int bMagnitude = 0, bShift = 0;
    if (proj) {
        // Orthographic: the a/c line maps Zs linearly; B_MAG/B_SHF are NOT
        // written (the SDK keeps their previous values; the ortho shader
        // formula does not use them).
        if ((farz == nearz) || (endz == startz)) {
            a = 0.0f;
            c = 0.0f;
        } else {
            const float inv = 1.0f / (endz - startz);
            a = (farz - nearz) * inv;
            c = (startz - nearz) * inv;
        }
    } else {
        // Perspective: A/B/C with B normalized into B_MAG (u0.24) + B_SHF.
        float A, B, C;
        if ((farz == nearz) || (endz == startz)) {
            A = 0.0f;
            B = 0.5f;
            C = 0.0f;
        } else {
            A = (farz * nearz) / ((farz - nearz) * (endz - startz));
            B = farz / (farz - nearz);
            C = startz / (endz - startz);
        }
        float bMant = B;
        int bExpn = 0;
        while (bMant > 1.0f) {
            bMant /= 2.0f;
            ++bExpn;
        }
        while ((bMant > 0.0f) && (bMant < 0.5f)) {
            bMant *= 2.0f;
            --bExpn;
        }
        bShift = bExpn + 1;
        bMagnitude = static_cast<int>(bMant * 8388638.0f);
        a = A / std::ldexp(1.0f, bShift);  // A / 2^bShift (safe vs int shift)
        c = C;
    }

    const std::uint8_t rgba[4] = {color.r, color.g, color.b, color.a};
    setFogState(a, c, bMagnitude, bShift, fsel, proj, rgba);
    PL_LOG_TRACE("gx", "GXSetFog(type %d, a=%g c=%g b_mag=%d b_shf=%d)", type, a, c,
                 bMagnitude, bShift);
}

void GXSetFogRangeAdj(GXBool enable, u16 center, const GXFogAdjTable* table) {
    setFogRangeAdjState(enable != GX_FALSE, center, table);
    PL_LOG_TRACE("gx", "GXSetFogRangeAdj(%d, %u)", enable != GX_FALSE,
                 static_cast<unsigned>(center));
}

} // extern "C"
