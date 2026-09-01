// =============================================================================
// M5.4: TEV combiner unit tests (CPU reference evaluator).
//
// Drives the GX TEV configuration API exactly like the game would, packs the
// state into the UBO (buildTevUbo) and evaluates the chain with the CPU
// reference (evalTevChain) against hand-computed values. The same formula is
// compiled into the TEV fragment shader (tools/shaders/gx_tev_frag.frag); the
// offscreen Vulkan test (video_test.cpp) verifies the GPU path end-to-end.
//
// Formula (Dolphin-verified, see gx.md §7):
//   lerp(a,b,c) = (((a<<8) + (b-a)·(c+(c>>7))) [+128/127]) >> 8   (scale 1)
//   out = (d+bias) ± lerp   — clamped [0,255] or [-1024,1023]
// All expected values below were computed by hand from this formula.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/gx/GXCompat.h"

namespace {

// Convenience: build the UBO from the current TEV state and evaluate with the
// given inputs.
void runTev(const Platform::CompatGx::TevChainInputs& in, int out[4]) {
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::evalTevChain(ubo, in, out);
}

Platform::CompatGx::TevChainInputs defaultInputs() {
    Platform::CompatGx::TevChainInputs in;
    in.ras[0][0] = 255; in.ras[0][1] = 128; in.ras[0][2] = 64; in.ras[0][3] = 255;
    in.ras[1][0] = 10; in.ras[1][1] = 20; in.ras[1][2] = 30; in.ras[1][3] = 40;
    in.texel[0][0] = 200; in.texel[0][1] = 100; in.texel[0][2] = 50; in.texel[0][3] = 255;
    in.texel[1][0] = 100; in.texel[1][1] = 200; in.texel[1][2] = 50; in.texel[1][3] = 128;
    return in;
}

} // namespace

// GXSetTevOp presets on stage 0 (RASC/RASA sources). Default tevorder binds
// TEXCOORD0/TEXMAP0/COLOR0A0, so ras = ras[0], texel = texel[0].
TEST_CASE(tev_op_presets_stage0) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();

    // MODULATE: out = lerp(ZERO, TEXC, RASC) = texel * ras / 256.
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], 200);  // 200·256/256
    CHECK_EQ(out[1], 50);   // 100·129/256 = 50.4 -> 50  (ras.g = 128)
    CHECK_EQ(out[2], 13);   // 50·64/256  = 12.5 -> 13   (ras.b = 64)
    CHECK_EQ(out[3], 255);

    // DECAL: out = lerp(RASC, TEXC, TEXA). Use TEXA = 128 for a half blend.
    auto decalIn = in;
    decalIn.texel[0][3] = 128;
    GXSetTevOp(GX_TEVSTAGE0, GX_DECAL);
    runTev(decalIn, out);
    CHECK_EQ(out[0], 227);  // (65280 - 55·129 + 128) >> 8
    CHECK_EQ(out[1], 114);  // (32768 - 28·129 + 128) >> 8
    CHECK_EQ(out[2], 57);   // (16384 - 14·129 + 128) >> 8
    CHECK_EQ(out[3], 255);  // alpha DECAL = RASA

    // BLEND: out = lerp(RASC, ONE, TEXC) = ras + texc·(255-ras)/256.
    GXSetTevOp(GX_TEVSTAGE0, GX_BLEND);
    runTev(in, out);
    CHECK_EQ(out[0], 255);  // lerp(255, 255, 200) = 255
    CHECK_EQ(out[1], 178);  // (32768 + 127·101 + 128) >> 8
    CHECK_EQ(out[2], 101);  // (16384 + 191·50 + 128) >> 8
    CHECK_EQ(out[3], 255);

    // REPLACE: out = TEXC (d = TEXC, lerp(0,0,0) = 0).
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    runTev(in, out);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 100);
    CHECK_EQ(out[2], 50);
    CHECK_EQ(out[3], 255);

    // PASSCLR: out = RASC (d = RASC).
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    runTev(in, out);
    CHECK_EQ(out[0], 255);
    CHECK_EQ(out[1], 128);
    CHECK_EQ(out[2], 64);
    CHECK_EQ(out[3], 255);
}

// GXSetTevOp presets on stages >= 1 (CPREV/APREV sources).
TEST_CASE(tev_op_presets_stage1) {
    GXInit(nullptr, 0);
    GXSetNumTevStages(2);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);  // prev = texel0 * ras0 / 256
    GXSetTevOp(GX_TEVSTAGE1, GX_MODULATE);  // prev = texel1 * prev / 256
    const auto in = defaultInputs();

    int out[4];
    runTev(in, out);
    // Stage 0: prev = (200, 50, 13, 255).
    // Stage 1: out = lerp(ZERO, TEXC, CPREV) = texel1 * prev / 256.
    CHECK_EQ(out[0], 79);  // 100·201/256
    CHECK_EQ(out[1], 39);  // 200·50/256   (c=50 -> c+(c>>7)=50)
    CHECK_EQ(out[2], 3);   // 50·13/256
    CHECK_EQ(out[3], 128); // 128·256/256
}

// Custom combiners: GXSetTevColorIn/AlphaIn/ColorOp/AlphaOp with bias, scale,
// clamp and dest registers.
TEST_CASE(tev_color_alpha_ops) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();

    // d = ONE (255), op ADD, bias ADDHALF (+128), scale 4, clamp -> saturates.
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_4, GX_TRUE,
                    GX_TEVPREV);
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], 255);  // ((255+128) << 2) = 1532 -> clamp 255
    CHECK_EQ(out[3], 255);  // alpha untouched (default REPLACE d=TEXA)

    // Same without clamp -> s10 range: 1532 -> 1023.
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_4, GX_FALSE,
                    GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[0], 1023);

    // DIVIDE_2 with d = HALF (128): (128 + 0) >> 1 = 64.
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_HALF);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE,
                    GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[0], 64);

    // SUB with scale 2: (128 << 1) - 0 = 256 -> clamp 255.
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE,
                    GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[0], 255);
}

// Register routing: dest C0/C1/C2 and the last-stage output copy to prev.
TEST_CASE(tev_register_routing) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();

    // Stage 0 writes d=ONE into C0 (not prev).
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVREG0);
    // Stage 1: out = C0 (the value stage 0 wrote); alpha forced to zero.
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetNumTevStages(2);
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], 255);  // stage 1 last, dest TEVPREV: prev.rgb = C0
    CHECK_EQ(out[1], 255);
    CHECK_EQ(out[2], 255);
    CHECK_EQ(out[3], 0);    // stage 1 alpha = 0

    // Last stage dest = C1: the result still goes to the screen (Dolphin
    // quirk) — prev gets C1's value.
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVREG1);
    runTev(in, out);
    CHECK_EQ(out[0], 255);
    CHECK_EQ(out[1], 255);
    CHECK_EQ(out[2], 255);
    CHECK_EQ(out[3], 0);
}

// TEVPREV initial register (GXSetTevColor(GX_TEVPREV)).
TEST_CASE(tev_prev_register) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();
    // Stage 0: out = d = CPREV/APREV -> passes the initial register through.
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevColor(GX_TEVPREV, GXColor{10, 20, 30, 40});
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], 10);
    CHECK_EQ(out[1], 20);
    CHECK_EQ(out[2], 30);
    CHECK_EQ(out[3], 40);
}

// Signed 10-bit color registers (GXSetTevColorS10) used as C0.
TEST_CASE(tev_color_s10) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();
    GXSetTevColorS10(GX_TEVREG0, GXColorS10{-100, 300, -512, 511});
    // out = C0 (negative values pass through the s10 range).
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE,
                    GX_TEVPREV);
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], -100);
    CHECK_EQ(out[1], 300);
    CHECK_EQ(out[2], -512);
    CHECK_EQ(out[3], 511);
}

// K constants: GXSetTevKColor + GXSetTevKColorSel/GXSetTevKAlphaSel.
TEST_CASE(tev_k_constants) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();
    GXSetTevKColor(GX_KCOLOR0, GXColor{200, 150, 100, 60});

    // out = KONST (K0 rgb, K0.a alpha).
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 150);
    CHECK_EQ(out[2], 100);
    CHECK_EQ(out[3], 60);

    // Component select: K0_R replicated into rgb.
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0_R);
    runTev(in, out);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 200);
    CHECK_EQ(out[2], 200);

    // Default KCSEL_1_4 / KASEL_1 (post-GXInit): rgb = 64, alpha = 255.
    GXInit(nullptr, 0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    runTev(in, out);
    CHECK_EQ(out[0], 64);
    CHECK_EQ(out[1], 64);
    CHECK_EQ(out[2], 64);
    CHECK_EQ(out[3], 255);
}

// Swap tables (GXSetTevSwapMode + GXSetTevSwapModeTable). Note the swap must
// be set AFTER GXSetTevOp — the preset writes SWAP0 into both fields.
TEST_CASE(tev_swap_tables) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();
    // tex swap SWAP2 (GGGA): textemp = (g,g,g,a); ras swap SWAP1 (RRRA).
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP1, GX_TEV_SWAP2);
    int out[4];
    runTev(in, out);
    // RASC = (255,255,255,255), TEXC = (100,100,100) -> out = texel swizzled.
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 100);
    CHECK_EQ(out[2], 100);
    CHECK_EQ(out[3], 255);  // TEXA = texel.a = 255

    // Custom table: SWAP1 = (B, B, R, G): TEXC = (50, 50, 200).
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_BLUE, GX_CH_BLUE, GX_CH_RED, GX_CH_GREEN);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1);
    runTev(in, out);
    CHECK_EQ(out[0], 50);
    CHECK_EQ(out[1], 50);
    CHECK_EQ(out[2], 200);
    CHECK_EQ(out[3], 100);  // TEXA = texel.g
}

// GXSetTevOrder: texmap clamping, disabled maps -> white, NULL color -> zero.
TEST_CASE(tev_order_semantics) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();

    // NULL map (disabled stage) + REPLACE -> textemp white -> out white.
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    int out[4];
    runTev(in, out);
    CHECK_EQ(out[0], 255);
    CHECK_EQ(out[1], 255);
    CHECK_EQ(out[2], 255);
    CHECK_EQ(out[3], 255);

    // GX_TEX_DISABLE (0x100) also disables the stage.
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEX_DISABLE, GX_COLOR0A0);
    runTev(in, out);
    CHECK_EQ(out[0], 255);
    CHECK_EQ(out[3], 255);

    // With zero texgens every enabled stage samples black (Dolphin rule).
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    runTev(in, out);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 0);
    CHECK_EQ(out[3], 0);

    // NULL color channel -> raster zero: PASSCLR outputs (0,0,0,0).
    GXSetNumTexGens(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    runTev(in, out);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[3], 0);

    // COLOR1 channel: PASSCLR outputs ras[1].
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR1A1);
    runTev(in, out);
    CHECK_EQ(out[0], 10);
    CHECK_EQ(out[1], 20);
    CHECK_EQ(out[2], 30);
    CHECK_EQ(out[3], 40);
}

// Comparison ops (GX_TEV_COMP_*).
TEST_CASE(tev_comparison_ops) {
    GXInit(nullptr, 0);
    const auto in = defaultInputs();
    GXSetTevColor(GX_TEVREG0, GXColor{100, 50, 200, 255});
    GXSetTevColor(GX_TEVREG1, GXColor{100, 100, 50, 255});
    // a=C0 b=C1 c=C0 d=ZERO.
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_C1, GX_CC_C0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    int out[4];
    runTev(in, out);
    // RGB8 GT: per channel — R 100!>100, G 50!>100, B 200>50 -> c=200.
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 200);

    // RGB8 EQ: R 100==100 -> c.r; G/B false -> 0.
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_EQ, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 0);

    // R8 GT: only the red channel is compared (100!>100) -> 0.
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 0);

    // R8 EQ: 100==100 -> c (=C0 rgb) everywhere.
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_R8_EQ, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 50);
    CHECK_EQ(out[2], 200);

    // Alpha A8 (compare mode RGB8 on the alpha combiner = A8): aA=100 > aB=50.
    GXSetTevColor(GX_TEVREG0, GXColor{0, 0, 0, 100});
    GXSetTevColor(GX_TEVREG1, GXColor{0, 0, 0, 50});
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_A1, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    runTev(in, out);
    CHECK_EQ(out[3], 100);  // d + aC = 0 + 100
}

// GXSetNumTevStages bounds.
TEST_CASE(tev_num_stages) {
    GXInit(nullptr, 0);
    GXSetNumTevStages(1);
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    CHECK_EQ(ubo.header[0], 1);
    GXSetNumTevStages(16);
    Platform::CompatGx::buildTevUbo(ubo);
    CHECK_EQ(ubo.header[0], 16);
    GXSetNumTevStages(0);  // out of range -> ignored
    Platform::CompatGx::buildTevUbo(ubo);
    CHECK_EQ(ubo.header[0], 16);
    CHECK_EQ(Platform::CompatGx::tevNumStages(), 16);
    CHECK_EQ(Platform::CompatGx::tevTexGenCount(), 1);
}

// =============================================================================
// M5.5: pixel-engine output stage (CPU reference evalPixelEngine).
//
// evalPixelEngine chains TEV -> &255 mask -> alpha compare (GXSetAlphaCompare)
// -> "alpha == 1" quirk -> fog (GXSetFog) -> final 0..255 RGBA. The same
// formulas run in gx_tev_frag.frag; the offscreen test (video_test.cpp) proves
// GPU == CPU end-to-end. Fog values below use zCoord = 8388608 (half of the
// 24-bit GX depth, i.e. gl_FragCoord.z = 0.5 with the identity MVP).
// =============================================================================

TEST_CASE(pe_alpha_compare_logic) {
    GXInit(nullptr, 0);
    // prev alpha = 100. comp0 = GREATER(64) passes, comp1 = LESS(128) passes.
    GXSetAlphaCompare(GX_GREATER, 64, GX_AOP_AND, GX_LESS, 128);
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::TevChainInputs in;
    in.texel[0][0] = 100; in.texel[0][1] = 100; in.texel[0][2] = 100; in.texel[0][3] = 100;
    bool discarded = false;
    std::int32_t out[4];
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, false);

    // OR of two failing compares still fails -> discard.
    GXSetAlphaCompare(GX_LESS, 64, GX_AOP_OR, GX_GREATER, 128);
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, true);

    // XOR: one pass, one fail -> pass.
    GXSetAlphaCompare(GX_LESS, 64, GX_AOP_XOR, GX_LESS, 128);  // fail XOR pass
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, false);

    // XNOR: both pass -> pass; both fail -> pass.
    GXSetAlphaCompare(GX_GREATER, 64, GX_AOP_XNOR, GX_LESS, 128);  // pass XNOR pass
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, false);
    GXSetAlphaCompare(GX_LESS, 64, GX_AOP_XNOR, GX_GREATER, 128);  // fail XNOR fail
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, false);

    // Default (GXInit): ALWAYS/0/AND/ALWAYS/0 always passes.
    GXInit(nullptr, 0);
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, false);
}

TEST_CASE(pe_fog_ortho_linear) {
    GXInit(nullptr, 0);
    // Ortho linear fog: a = (far-near)/(end-start) = (9-1)/(8-2) = 8/6,
    // c = (start-near)/(end-start) = 1/6 (both truncated to 20-bit registers
    // like the hardware). With zCoord = 8388608:
    //   ze = a * 8388608/16777216 = a/2 -> fog = a/2 - c ≈ 0.4999 -> ifog = 128.
    GXSetFog(GX_FOG_ORTHO_LIN, 2.0f, 8.0f, 1.0f, 9.0f, GXColor{255, 128, 64, 255});
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::TevChainInputs in;
    in.texel[0][0] = 100; in.texel[0][1] = 150; in.texel[0][2] = 200; in.texel[0][3] = 255;
    bool discarded = false;
    std::int32_t out[4];
    Platform::CompatGx::evalPixelEngine(ubo, in, 8388608, discarded, out);
    CHECK_EQ(discarded, false);
    // (prev*128 + fog*128) >> 8 with prev = (100,150,200): (177,139,132).
    CHECK_EQ(out[0], 177);
    CHECK_EQ(out[1], 139);
    CHECK_EQ(out[2], 132);
    CHECK_EQ(out[3], 255);  // fog affects RGB only
}

TEST_CASE(pe_fog_off) {
    // GXInit leaves fog NONE (fsel 0): output = TEV output masked to 8 bits.
    GXInit(nullptr, 0);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::TevChainInputs in;
    in.texel[0][0] = 10; in.texel[0][1] = 20; in.texel[0][2] = 30; in.texel[0][3] = 40;
    bool discarded = false;
    std::int32_t out[4];
    Platform::CompatGx::evalPixelEngine(ubo, in, 0, discarded, out);
    CHECK_EQ(discarded, false);
    CHECK_EQ(out[0], 10);
    CHECK_EQ(out[1], 20);
    CHECK_EQ(out[2], 30);
    CHECK_EQ(out[3], 40);
}
