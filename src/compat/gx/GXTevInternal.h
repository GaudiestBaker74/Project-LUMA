#pragma once
// =============================================================================
// compat/gx — TEV + pixel-engine state (M5.4/M5.5) internal interface.
//
// GXTev.cpp mirrors the GX TEV state (GXSetTevOp/ColorIn/ColorOp/Color/Order/
// KColor/SwapMode...) and the fragment-side pixel-engine state that packs into
// the fragment UBO (fog + alpha compare), then packs the mirror into a std140
// UBO consumed by the TEV fragment shader (tools/shaders/gx_tev_frag.frag).
// This header also exposes the CPU reference evaluators used by the unit
// tests: evalTevChain implements the exact per-stage TEV formula, and
// evalPixelEngine chains TEV -> mask -> alpha test -> fog, i.e. the whole
// fragment pipeline; both must stay in lockstep with the shader (verified
// end-to-end by the offscreen Vulkan test in video_test.cpp).
//
// Layout note: TevUboData MUST stay byte-identical to the GLSL UBO block
// (std140). Do not reorder fields without updating the shader.
// =============================================================================

#include <cstdint>

namespace Platform::CompatGx {

inline constexpr int kTevMaxStages = 16;
inline constexpr int kTevMaxTexmaps = 8;
inline constexpr int kTevMaxTexcoords = 8;

// ---- op-pack helpers (shared bit layout with gx_tev_frag.frag) --------------
// colorOpPack (int32): op(0-4) | bias(5-6) | scale(7-8) | clamp(9) | dest(10-11)
// alphaOpPack (int32): op(0-4) | bias(5-6) | scale(7-8) | clamp(9) | dest(10-11)
//                      | rasSel(12-13) | texSel(14-15)
// orderPack (int32):   texmap(0-7) | texcoord(8-15) | colorChan(16-23) | texEnable(24)
// kselPack (int32):    kColorSel(0-7) | kAlphaSel(8-15)
inline int packColorOp(int op, int bias, int scale, int clamp, int dest) {
    return op | (bias << 5) | (scale << 7) | (clamp << 9) | (dest << 10);
}
inline int packAlphaOp(int op, int bias, int scale, int clamp, int dest, int rasSel,
                       int texSel) {
    return op | (bias << 5) | (scale << 7) | (clamp << 9) | (dest << 10) |
           (rasSel << 12) | (texSel << 14);
}
inline int packOrder(int texmap, int texcoord, int colorChan, int texEnable) {
    return texmap | (texcoord << 8) | (colorChan << 16) | (texEnable << 24);
}
inline int packKsel(int kColorSel, int kAlphaSel) {
    return kColorSel | (kAlphaSel << 8);
}

// ---- UBO data (std140 layout, see shader) ------------------------------------
struct alignas(16) TevUboData {
    // Initial TEVPREV color register (RGBA, signed 10-bit values).
    std::int32_t prev[4];
    // C0..C2 color registers (RGBA, signed 10-bit values).
    std::int32_t tevReg[3][4];
    // K0..K3 constant colors (RGBA, 0..255).
    std::int32_t konst[4][4];
    struct alignas(16) Stage {
        std::int32_t colorEnv[4];   // color combiner a,b,c,d (GXTevColorArg)
        std::int32_t alphaEnv[4];   // alpha combiner a,b,c,d (GXTevAlphaArg)
        std::int32_t opParams[4];   // colorOpPack, alphaOpPack, orderPack, kselPack
        std::int32_t reserved[4];   // unused
    } stage[kTevMaxStages];
// header: [0] = num stages, [1] = num texgens, [2..3] = unused.
std::int32_t header[4];
    // swapTables[t][0..3] = r,g,b,a channel indices for swap table t.
    std::int32_t swapTables[4][4];
    // --- M5.5 pixel-engine extras (fog + alpha compare) -----------------------
    // Fog color RGB (0..255); alpha unused.
    std::int32_t fogColor[4];
    // fogReg: [0] = b_magnitude (u0.24 integer, B_MAG register), [1] = b_shift
    // (B_SHF register, exponent+1), [2] = fsel (0..7), [3] = proj (0/1).
    std::int32_t fogReg[4];
    // fogAB: [0] = A (float, decoded 20-bit TEV_FOG_PARAM_0), [1] = C (float,
    // decoded TEV_FOG_PARAM_3). The low 12 mantissa bits of the original
    // float are zeroed exactly like the hardware register.
    float fogAB[4];
    // alphaCmp: [0] = comp0(3) | ref0(8) | logic(2) | comp1(3) | ref1(8) of
    // GXSetAlphaCompare. The shader always evaluates both compares and combines
    // them with the logic op (GX_AOP_AND/OR/XOR/XNOR); GXInit's ALWAYS/0/AND/
    // ALWAYS/0 therefore always passes.
    std::int32_t alphaCmp[4];
    // --- M5.7b indirect stages -----------------------------------------------
    // indParams[i].x = baseCoord | indMap<<8 | scaleS<<16 | scaleT<<20.
    std::int32_t indParams[4][4];
    // Two rows per indirect matrix: (m0,m1,m2, 128/2^scaleExp).
    float indMtx[3][2][4];
    // texDims[map] = (w, h, 0, 0): texel size of the stage's direct map.
    float texDims[8][4];
};
static_assert(sizeof(TevUboData) == 1584, "TevUboData must match the shader UBO");

// ---- CPU reference evaluators -------------------------------------------------
// Inputs to a TEV chain evaluation. All values are the *unswizzled* sources:
// raster colors (from vertex varyings) and sampled texels, 0..255. The number
// of texgens (black rule) is read from the packed ubo.header[1].
struct TevChainInputs {
    std::int32_t ras[2][4] = {};                    // vertex raster colors 0..255
    std::int32_t texel[kTevMaxTexmaps][4] = {};     // sampled texels 0..255
};

// Evaluates the full TEV chain described by `ubo` (num stages = ubo.header[0])
// with the given inputs. `out` receives the final TEVPREV RGBA (0..255 range
// when clamped; otherwise s10 range).
void evalTevChain(const TevUboData& ubo, const TevChainInputs& in, std::int32_t out[4]);

// Evaluates the whole fragment pipeline (mirror of gx_tev_frag.frag main):
// TEV chain -> &255 mask -> alpha test (sets `discarded`; the shader discards
// the fragment) -> "alpha == 1" blend quirk -> fog (zCoord is the 24-bit GX
// depth, 0..0xFFFFFF). `out` receives the final 0..255 RGBA sent to the
// framebuffer. Fog range adjustment (GXSetFogRangeAdj) is not applied yet
// (TODO M5.7) — same as the shader.
void evalPixelEngine(const TevUboData& ubo, const TevChainInputs& in, int zCoord,
                     bool& discarded, std::int32_t out[4]);

// ---- PC hooks used by GXCompat.cpp -------------------------------------------
// Resets the TEV + pixel-engine state mirror to the post-GXInit defaults
// (see docs/gx.md §7).
void resetTevState();

// Packs the current state mirror into `out` (UBO layout). Called by
// GXCompat::flushDraw before uploadFragmentUbo.
void buildTevUbo(TevUboData& out);

// Number of active TEV stages / texgens (header values for buildTevUbo).
int tevNumStages();
int tevTexGenCount();

// Stores the fog parameters computed by GXSetFog (mirror of the vendored
// GXPixel.c computation). `a`/`c` are the float values BEFORE the 20-bit
// register packing; the mantissa truncation is applied here exactly like the
// hardware (TEV_FOG_PARAM_0/3 keep only the top 11 fraction bits).
// `bMagnitude`/`bShift` are the raw B_MAG (u0.24) and B_SHF registers (persp
// only; ortho leaves them unchanged — mirror the SDK, which only writes them
// in the perspective branch).
void setFogState(float a, float c, std::int32_t bMagnitude, std::int32_t bShift,
                 std::int32_t fsel, std::int32_t proj, const std::uint8_t color[4]);

// Stores the GXSetFogRangeAdj state (enable/center/table). M5.5 stores it for
// API completeness; the shader/UBO do not consume it yet (TODO M5.7: fog range
// adjustment).
void setFogRangeAdjState(bool enable, int center, const void* table);

// Stores the GXSetAlphaCompare state (comp0/ref0/logic/comp1/ref1).
void setAlphaCompareState(std::int32_t comp0, std::int32_t ref0, std::int32_t logic,
                          std::int32_t comp1, std::int32_t ref1);

// ---- M5.6: display-list register application (raw BP writes) ----------------
// The display-list interpreter (GXCompat.cpp) decodes the raw 24-bit BP
// register values and routes the TEV/PE-family registers here so the same
// mirror that the GXSet* APIs drive is updated (a DL can carry baked state,
// e.g. TEV combiners, fog or the alpha compare — the J3D "GD" path writes
// these registers straight into the DL).

// Applies the GEN_MODE register (RID 0x00): the num-tev-stages (bits 10-13)
// and num-texgens (bits 0-3) fields. The cull-mode field (bits 14-15) is
// handled by GXCompat.cpp (its own mirror).
void dlApplyGenMode(std::uint32_t value);

// Applies one TEV/PE-family BP register to the TEV mirror. `rid` is the
// register id (bits 24-31 of the raw write), `value` the already-masked
// 24-bit payload. Supported: TEVC/TEVA (0xC0+2i), TEV_REGISTERL/H (0xE0+2i,
// the bit-23 type selects color vs constant), FOG (0xEE..0xF2), ALPHAFUNC
// (0xF3), KSEL (0xF6+i), TREF (0x28+i). Everything else is logged and
// ignored (mirror unchanged) — see docs/gx.md §M5.6.
void dlApplyBpTev(std::uint8_t rid, std::uint32_t value);

} // namespace Platform::CompatGx
