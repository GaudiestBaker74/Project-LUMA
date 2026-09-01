// =============================================================================
// M5.1: GX immediate-vertex capture (headless — no renderer needed).
//
// Drives the compat/gx state machine exactly like the game would and verifies
// the captured vertex stream: GXSetVtxDesc + GXSetVtxAttrFmt define the vertex
// descriptor (VCD) and attribute formats (VAT); GXBegin + the immediate
// writers (GXPosition3f32/GXColor4u8) push into the simulated write-gather
// pipe; the completed primitive is serialized to floats in VCD order.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/gx/GXCompat.h"
#include "compat/gx/GXLightInternal.h"
#include "compat/gx/GXIndInternal.h"

// GXLightObjInt (the layout of GXLightObj) for the spot-light test below.
#include <revolution/gx/GXTypes.h>

TEST_CASE(gx_immediate_vertex_capture) {
    GXInit(nullptr, 0);

    // VCD: position (direct) + color0 (direct).
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);

    // VAT on VTXFMT0: pos = XYZ f32, color0 = RGBA (u8x4).
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    // A quad: 4 vertices, each pos(3) + color(4) = 7 floats.
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(0.0f, 0.0f, 0.0f);
    GXColor4u8(255, 0, 0, 255);
    GXPosition3f32(1.0f, 0.0f, 0.0f);
    GXColor4u8(0, 255, 0, 255);
    GXPosition3f32(1.0f, 1.0f, 0.0f);
    GXColor4u8(0, 0, 255, 255);
    GXPosition3f32(0.0f, 1.0f, 0.0f);
    GXColor4u8(255, 255, 0, 255);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 4);
    CHECK(stride == 7); // 3 pos + 4 color

    // Vertex 0: position exact, color u8 -> /255.
    CHECK_NEAR(data[0], 0.0f, 1e-6f);
    CHECK_NEAR(data[1], 0.0f, 1e-6f);
    CHECK_NEAR(data[2], 0.0f, 1e-6f);
    CHECK_NEAR(data[3], 1.0f, 1e-4f); // r
    CHECK_NEAR(data[4], 0.0f, 1e-4f); // g
    CHECK_NEAR(data[5], 0.0f, 1e-4f); // b
    CHECK_NEAR(data[6], 1.0f, 1e-4f); // a

    // Vertex 2 (stride 7): position (1,1,0), color (0,0,1,1).
    CHECK_NEAR(data[14], 1.0f, 1e-6f);
    CHECK_NEAR(data[15], 1.0f, 1e-6f);
    CHECK_NEAR(data[17], 0.0f, 1e-4f);
    CHECK_NEAR(data[18], 0.0f, 1e-4f);
    CHECK_NEAR(data[19], 1.0f, 1e-4f);
}

TEST_CASE(gx_packed_color_u32) {
    // GXColor1u32 packs the 4 RGBA8 bytes into ONE 32-bit FIFO word; the
    // vertex loader still consumes 4 color components from it (big-endian:
    // r = MSB ... a = LSB).
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition3f32(1.0f, 2.0f, 3.0f);
    GXColor1u32(0xFF0000FFu); // r=255, g=0, b=0, a=255
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 1);
    CHECK(stride == 7);
    CHECK_NEAR(data[0], 1.0f, 1e-6f);
    CHECK_NEAR(data[3], 1.0f, 1e-4f); // r
    CHECK_NEAR(data[4], 0.0f, 1e-4f); // g
    CHECK_NEAR(data[5], 0.0f, 1e-4f); // b
    CHECK_NEAR(data[6], 1.0f, 1e-4f); // a
}

TEST_CASE(gx_vcd_with_texcoord) {
    // VCD pos + clr0 + tex0: the serialized stride is 3+4+2 = 9 floats; the
    // texcoords are captured even though the M5.1 shader does not consume them.
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition3f32(0.0f, 0.0f, 0.0f);
    GXColor4u8(255, 255, 255, 255);
    GXTexCoord2f32(0.25f, 0.75f);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 1);
    CHECK(stride == 9);
    CHECK_NEAR(data[7], 0.25f, 1e-6f); // tex s
    CHECK_NEAR(data[8], 0.75f, 1e-6f); // tex t
}

TEST_CASE(gx_attr_fmt_scaling) {
    // Fixed-point attributes scale by 2^frac: a S16 value 0x0100 with frac 8
    // is 1.0f. Exercises the VAT conversion path.
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition3s16(0x0100, 0x0200, 0x0400);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 1);
    CHECK(stride == 3);
    CHECK_NEAR(data[0], 1.0f, 1e-4f);
    CHECK_NEAR(data[1], 2.0f, 1e-4f);
    CHECK_NEAR(data[2], 4.0f, 1e-4f);
}

// =============================================================================
// M5.2: indexed attributes (GXSetArray + GX_INDEX8/16).
// =============================================================================

TEST_CASE(gx_index16_pos_array) {
    // Position data in a GX array, referenced with 16-bit indices.
    GXInit(nullptr, 0);
    const float pos[2][3] = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetArray(GX_VA_POS, pos, sizeof(pos[0]));

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition1x16(0);
    GXPosition1x16(1);
    GXPosition1x16(0);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 3);
    CHECK(stride == 3);
    CHECK_NEAR(data[0], 1.0f, 1e-6f);
    CHECK_NEAR(data[2], 3.0f, 1e-6f);
    CHECK_NEAR(data[3], 4.0f, 1e-6f);
    CHECK_NEAR(data[5], 6.0f, 1e-6f);
    CHECK_NEAR(data[6], 1.0f, 1e-6f); // index 0 again
}

TEST_CASE(gx_index8_mixed_direct_indexed) {
    // Mixed VCD: position DIRECT (raw FIFO values), color0 INDEX8 (array).
    GXInit(nullptr, 0);
    const u8 col[2][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}};
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetArray(GX_VA_CLR0, col, sizeof(col[0]));

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition2f32(3.0f, 4.0f);
    GXColor1x8(0);
    GXPosition2f32(5.0f, 6.0f);
    GXColor1x8(1);
    GXPosition2f32(7.0f, 8.0f);
    GXColor1x8(0);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 3);
    CHECK(stride == 6); // 2 pos + 4 color
    // Vertex 0: pos (3,4), color red.
    CHECK_NEAR(data[0], 3.0f, 1e-6f);
    CHECK_NEAR(data[2], 1.0f, 1e-4f);
    CHECK_NEAR(data[4], 0.0f, 1e-4f);
    // Vertex 1: pos (5,6), color green.
    CHECK_NEAR(data[6], 5.0f, 1e-6f);
    CHECK_NEAR(data[9], 1.0f, 1e-4f);
    // Vertex 2: pos (7,8), color red again (index 0).
    CHECK_NEAR(data[12], 7.0f, 1e-6f);
    CHECK_NEAR(data[14], 1.0f, 1e-4f);
}

TEST_CASE(gx_indexed_array_stride) {
    // Arrays may have a stride larger than the attribute (interleaved
    // buffers): the fetch must honor GXSetArray's stride.
    GXInit(nullptr, 0);
    struct Vtx {
        float x, y, z;
        float pad;
    };
    const Vtx v[2] = {{1, 2, 3, 99}, {4, 5, 6, 99}};
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetArray(GX_VA_POS, v, sizeof(Vtx)); // 16-byte stride

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 2);
    GXPosition1x16(0);
    GXPosition1x16(1);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 2);
    CHECK_NEAR(data[0], 1.0f, 1e-6f);
    CHECK_NEAR(data[2], 3.0f, 1e-6f);
    CHECK_NEAR(data[3], 4.0f, 1e-6f);
    CHECK_NEAR(data[5], 6.0f, 1e-6f);
}

TEST_CASE(gx_indexed_frac_scaling) {
    // Fixed-point S16 array with frac 8: 0x0100 -> 1.0f.
    GXInit(nullptr, 0);
    const s16 pos[2][3] = {{0x0100, 0x0200, 0x0400}, {0x0800, 0x1000, 0x2000}};
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
    GXSetArray(GX_VA_POS, pos, sizeof(pos[0]));

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition1x16(0);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 1);
    CHECK_NEAR(data[0], 1.0f, 1e-4f);
    CHECK_NEAR(data[1], 2.0f, 1e-4f);
    CHECK_NEAR(data[2], 4.0f, 1e-4f);
}

TEST_CASE(gx_clear_vtx_desc) {
    // GXClearVtxDesc drops every attribute: the VCD is empty and the vertex
    // writes are ignored (no capture).
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3f32(1.0f, 2.0f, 3.0f);
    GXPosition3f32(4.0f, 5.0f, 6.0f);
    GXPosition3f32(7.0f, 8.0f, 9.0f);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    CHECK(data == nullptr); // nothing captured
    CHECK(count == 0);
}

TEST_CASE(gx_set_vtx_attr_fmtv) {
    // GXSetVtxAttrFmtv applies a NULL-terminated list of formats.
    GXInit(nullptr, 0);
    const GXVtxAttrFmtList list[] = {
        {GX_VA_POS, GX_POS_XYZ, GX_F32, 0},
        {GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0},
        {GX_VA_NULL, GX_POS_XYZ, GX_F32, 0}, // terminator
    };
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmtv(GX_VTXFMT0, list);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition3f32(1.0f, 2.0f, 3.0f);
    GXColor4u8(10, 20, 30, 40);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 1);
    CHECK(stride == 7);
    CHECK_NEAR(data[3], 10.0f / 255.0f, 1e-4f);
    CHECK_NEAR(data[6], 40.0f / 255.0f, 1e-4f);
}

// =============================================================================
// M5.3: texcoord generation (GXSetTexCoordGen2 + GXLoadTexMtxImm).
// =============================================================================

TEST_CASE(gx_texcoord_gen_matrix) {
    // GX_TG_MTX2x4 with a stored tex matrix transforms the vertex texcoord
    // on the CPU (uv' = M * uv).
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    // Row-major 2x4: u' = 2u + 1, v' = 2v + 3.
    const f32 m[2][4] = {{2.0f, 0.0f, 0.0f, 1.0f},
                         {0.0f, 2.0f, 0.0f, 3.0f}};
    GXLoadTexMtxImm(m, GX_TEXMTX0, GX_MTX2x4);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE,
                      GX_PTIDENTITY);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition3f32(1.0f, 2.0f, 3.0f);
    GXTexCoord2f32(0.5f, 0.25f);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(count == 1);
    CHECK(stride == 5); // pos3 + tex0 2
    CHECK_NEAR(data[0], 1.0f, 1e-6f);
    CHECK_NEAR(data[3], 2.0f, 1e-5f); // 2*0.5 + 1
    CHECK_NEAR(data[4], 3.5f, 1e-5f); // 2*0.25 + 3
}

TEST_CASE(gx_texcoord_gen_identity) {
    // GX_IDENTITY matrix: uv passes through unchanged.
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE,
                      GX_PTIDENTITY);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition2f32(0.0f, 0.0f);
    GXTexCoord2f32(0.75f, 0.125f);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK(stride == 4);
    CHECK_NEAR(data[2], 0.75f, 1e-6f);
    CHECK_NEAR(data[3], 0.125f, 1e-6f);
}

TEST_CASE(gx_texcoord_gen_from_position) {
    // GX_TG_MTX3x4 with GX_TG_POS source: uv = M * position (projected by w).
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    // u = x, v = y, w = 2 -> uv = (x/2, y/2).
    const f32 m[3][4] = {{1, 0, 0, 0},
                         {0, 1, 0, 0},
                         {0, 0, 0, 2}};
    GXLoadTexMtxImm(m, GX_TEXMTX1, GX_MTX3x4);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX1, GX_FALSE,
                      GX_PTIDENTITY);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition3f32(4.0f, 6.0f, 0.0f);
    GXTexCoord2f32(0.0f, 0.0f); // ignored: generated from POS
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_NEAR(data[3], 2.0f, 1e-5f); // 4/2
    CHECK_NEAR(data[4], 3.0f, 1e-5f); // 6/2
}

TEST_CASE(gx_texcoord_no_gen_passthrough) {
    // Without GXSetTexCoordGen2 the vertex texcoord is passed through.
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 1);
    GXPosition2f32(1.0f, 2.0f);
    GXTexCoord2f32(0.1f, 0.9f);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_NEAR(data[2], 0.1f, 1e-6f);
    CHECK_NEAR(data[3], 0.9f, 1e-6f);
}

// =============================================================================
// M5.5: pixel-engine state mirror (GXSetZMode/BlendMode/DstAlpha/AlphaCompare/
// Fog -> GXCompatDebugPeState). Headless — the mirror drives the renderer's
// PipelineDesc at draw time; these tests pin the GX API -> state mapping.
// =============================================================================

TEST_CASE(gx_pixel_engine_state) {
    // GXInit applies the console reset (vendored GXInit.c order):
    // cull BACK, blend NONE(SRCALPHA,INVSRCALPHA,CLEAR), updates ENABLE,
    // z TRUE/LEQUAL/TRUE, ZCompLoc TRUE, dstalpha DISABLE, dither ENABLE,
    // pixelFmt RGB8_Z24 / ZC_LINEAR.
    GXInit(nullptr, 0);
    GxPeDebugState st{};
    GXCompatDebugPeState(st);
    CHECK_EQ(st.cullMode, GX_CULL_BACK);
    CHECK_EQ(st.blendMode, GX_BM_NONE);
    CHECK_EQ(st.blendSrc, GX_BL_SRCALPHA);
    CHECK_EQ(st.blendDst, GX_BL_INVSRCALPHA);
    CHECK_EQ(st.blendLogicOp, GX_LO_CLEAR);
    CHECK_EQ(st.zTest, 1);
    CHECK_EQ(st.zFunc, GX_LEQUAL);
    CHECK_EQ(st.zWrite, 1);
    CHECK_EQ(st.zCompLoc, 1);
    CHECK_EQ(st.colorUpdate, 1);
    CHECK_EQ(st.alphaUpdate, 1);
    CHECK_EQ(st.dstAlphaEnable, 0);
    CHECK_EQ(st.dstAlphaValue, 0);
    CHECK_EQ(st.dither, 1);
    CHECK_EQ(st.pixelFmt, GX_PF_RGB8_Z24);
    CHECK_EQ(st.zFormat, GX_ZC_LINEAR);

    // GXSetBlendMode(BLEND, SRCALPHA, ONE, NOOP): the mirror keeps the exact
    // GX enums; flushDraw maps them (SRCALPHA -> SRC_ALPHA etc.).
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.blendMode, GX_BM_BLEND);
    CHECK_EQ(st.blendSrc, GX_BL_SRCALPHA);
    CHECK_EQ(st.blendDst, GX_BL_ONE);
    CHECK_EQ(st.blendLogicOp, GX_LO_NOOP);

    // GXSetBlendMode(SUBTRACT, ...) keeps the factors; flushDraw maps
    // GX_BM_SUBTRACT to ReverseSubtract.
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.blendMode, GX_BM_SUBTRACT);

    // GXSetBlendMode(LOGIC, ...): logic op replaces blending in flushDraw.
    GXSetBlendMode(GX_BM_LOGIC, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_XOR);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.blendMode, GX_BM_LOGIC);
    CHECK_EQ(st.blendLogicOp, GX_LO_XOR);

    // Z mode / Z compare location.
    GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.zTest, 1);
    CHECK_EQ(st.zFunc, GX_GEQUAL);
    CHECK_EQ(st.zWrite, 0);
    GXSetZCompLoc(GX_FALSE);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.zCompLoc, 0);

    // Color/alpha update.
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_FALSE);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.colorUpdate, 0);
    CHECK_EQ(st.alphaUpdate, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);

    // Dst alpha (constant alpha used by the CONSTANT_ALPHA blend factor).
    GXSetDstAlpha(GX_TRUE, 0x80);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.dstAlphaEnable, 1);
    CHECK_EQ(st.dstAlphaValue, 0x80);

    // Dither + pixel format.
    GXSetDither(GX_FALSE);
    GXSetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    GXCompatDebugPeState(st);
    CHECK_EQ(st.dither, 0);
    CHECK_EQ(st.pixelFmt, GX_PF_RGB565_Z16);
}

// =============================================================================
// M5.6: display-list interpreter (GXCallDisplayList).
//
// Builds display lists with the exact GD writers the game uses
// (GDWriteCPCmd / GDWriteBPCmd / GDWriteXFCmd / GDWrite_u8/u16/u32/f32 — the
// big-endian FIFO byte stream) and verifies the interpreter applies CP
// (VCD/VAT/arrays), BP (GEN_MODE/ZMODE/BLENDMODE/TEV) and XF (texgen) state
// and replays primitives through the same vertex machinery as the immediate
// path. State applications are verified two ways: PE mirrors via
// GXCompatDebugPeState, and the TEV mirror by comparing the UBO produced from
// a DL against the UBO produced from the equivalent GXSet* API calls.
// =============================================================================

#include <revolution/gd/GDBase.h>
#include <revolution/gd/GDGeometry.h>
#include <revolution/gd/GDLight.h>
#include <revolution/gx/GXRegs.h>  // private/*.h bit layouts

namespace {

// Minimal DL recorder: replicates GDInitGDLObj/GDPadCurr32 (GDBase.c, not
// compiled into the PC build) so tests record lists with the same GDWrite*
// writers the game uses. GDSetCurrent is an inline in GDBase.h.
struct DlBuilder {
    alignas(32) u8 buf[1024];
    GDLObj obj;

    void begin() {
        obj.start = buf;
        obj.ptr = buf;
        obj.top = buf + sizeof(buf);
        obj.length = sizeof(buf);
        GDSetCurrent(&obj);
    }
    u32 end() {
        while ((reinterpret_cast<uintptr_t>(obj.ptr) & 31) != 0) {
            GDWrite_u8(0);
        }
        const u32 size = static_cast<u32>(obj.ptr - obj.start);
        GDSetCurrent(nullptr);
        return size;
    }
};

// Inserts `val` into `reg` at [shift, shift+size) (the SC_* shortcut macros
// are no-ops on non-MWERKS toolchains, so the tests build raw register
// values with the vendored *_SHIFT/*_SIZE constants).
u32 put(u32 reg, u32 val, int shift, int size) {
    const u32 mask = (size >= 32) ? 0xFFFFFFFFu : ((1u << size) - 1u);
    return (reg & ~(mask << shift)) | ((val & mask) << shift);
}

// The physical base the console bakes into DL ARRAY_BASE commands
// (GX_PHY_ADDR = (u32)ptr & 0x3FFFFFFF).
u32 phyOf(const void* p) {
    return static_cast<u32>(reinterpret_cast<uintptr_t>(p)) & 0x3FFFFFFFu;
}

// VCD "position + color0, both direct" (the common baked format).
void writeVcdPosClr(DlBuilder& dl) {
    GDWriteCPCmd(CP_VCD_LO_ID,
                 CP_VCD_REG_LO(0, 0, 0, 0, 0, 0, 0, 0, 0, GX_DIRECT, GX_NONE,
                               GX_DIRECT, GX_NONE));
    GDWriteCPCmd(CP_VAT_A_ID, CP_REG_VAT_GRP0(GX_POS_XYZ, GX_F32, 0, GX_NRM_XYZ,
                                              GX_F32, GX_CLR_RGBA, GX_RGBA8,
                                              GX_CLR_RGBA, GX_RGBA8, GX_TEX_ST,
                                              GX_F32, 0, 1, 0));
}

void writePosClrVertex(const float x, const float y, const float z, const u8 r,
                       const u8 g, const u8 b, const u8 a) {
    GDWrite_f32(x);
    GDWrite_f32(y);
    GDWrite_f32(z);
    GDWrite_u8(r);
    GDWrite_u8(g);
    GDWrite_u8(b);
    GDWrite_u8(a);
}

} // namespace

TEST_CASE(gx_dl_triangles_direct) {
    GXInit(nullptr, 0);

    DlBuilder dl;
    dl.begin();
    writeVcdPosClr(dl);
    GDWrite_u8(GX_TRIANGLES | GX_VTXFMT0);
    GDWrite_u16(3);
    writePosClrVertex(0.0f, 0.0f, 0.0f, 255, 0, 0, 255);
    writePosClrVertex(1.0f, 0.0f, 0.0f, 0, 255, 0, 255);
    writePosClrVertex(0.0f, 1.0f, 0.0f, 0, 0, 255, 255);
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_EQ(count, 3);
    CHECK_EQ(stride, 7);  // 3 pos + 4 color

    // Vertex 0: position exact, color u8 -> /255.
    CHECK_NEAR(data[0], 0.0f, 1e-6f);
    CHECK_NEAR(data[1], 0.0f, 1e-6f);
    CHECK_NEAR(data[2], 0.0f, 1e-6f);
    CHECK_NEAR(data[3], 1.0f, 1e-4f);  // r
    CHECK_NEAR(data[4], 0.0f, 1e-4f);  // g
    CHECK_NEAR(data[5], 0.0f, 1e-4f);  // b
    CHECK_NEAR(data[6], 1.0f, 1e-4f);  // a
    // Vertex 2 (stride 7): position (0,1,0), color (0,0,1,1).
    CHECK_NEAR(data[14], 0.0f, 1e-6f);
    CHECK_NEAR(data[15], 1.0f, 1e-6f);
    CHECK_NEAR(data[17], 0.0f, 1e-4f);
    CHECK_NEAR(data[18], 0.0f, 1e-4f);
    CHECK_NEAR(data[19], 1.0f, 1e-4f);
}

TEST_CASE(gx_dl_indexed_arrays) {
    GXInit(nullptr, 0);

    // A triangle registered with the API (the host side of the DL's
    // ARRAY_BASE/ARRAY_STRIDE physical addresses).
    const float pos[3][3] = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    GXSetArray(GX_VA_POS, pos, sizeof(pos[0]));

    DlBuilder dl;
    dl.begin();
    // VCD: position INDEX16 only.
    GDWriteCPCmd(CP_VCD_LO_ID,
                 CP_VCD_REG_LO(0, 0, 0, 0, 0, 0, 0, 0, 0, GX_INDEX16, GX_NONE,
                               GX_NONE, GX_NONE));
    GDWriteCPCmd(CP_VAT_A_ID, CP_REG_VAT_GRP0(GX_POS_XYZ, GX_F32, 0, GX_NRM_XYZ,
                                              GX_F32, GX_CLR_RGBA, GX_RGBA8,
                                              GX_CLR_RGBA, GX_RGBA8, GX_TEX_ST,
                                              GX_F32, 0, 1, 0));
    // Array base/stride carry the physical address of the pos array.
    GDWriteCPCmd(CP_ARRAY_BASE_ID + 0, phyOf(pos));
    GDWriteCPCmd(CP_ARRAY_STRIDE_ID + 0, sizeof(pos[0]));
    // One triangle via indices.
    GDWrite_u8(GX_TRIANGLES | GX_VTXFMT0);
    GDWrite_u16(3);
    GDWrite_u16(0);
    GDWrite_u16(1);
    GDWrite_u16(2);
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_EQ(count, 3);
    CHECK_EQ(stride, 3);  // pos only
    // v0 = pos[0] = (0,0,0), v1 = pos[1] = (2,0,0), v2 = pos[2] = (0,2,0).
    CHECK_NEAR(data[0], 0.0f, 1e-6f);
    CHECK_NEAR(data[1], 0.0f, 1e-6f);
    CHECK_NEAR(data[2], 0.0f, 1e-6f);
    CHECK_NEAR(data[3], 2.0f, 1e-6f);
    CHECK_NEAR(data[4], 0.0f, 1e-6f);
    CHECK_NEAR(data[5], 0.0f, 1e-6f);
    CHECK_NEAR(data[6], 0.0f, 1e-6f);
    CHECK_NEAR(data[7], 2.0f, 1e-6f);
    CHECK_NEAR(data[8], 0.0f, 1e-6f);
}

TEST_CASE(gx_dl_bp_state_mirrors_pe) {
    GXInit(nullptr, 0);

    DlBuilder dl;
    dl.begin();
    // GEN_MODE: 2 texgens, 2 TEV stages (field = n-1), cull BACK (hw 1).
    GDWriteBPCmd(BP_GEN_MODE(2, 0, 1, 1, 0));
    // ZMODE: test on, func GEQUAL, update off.
    u32 zmode = 0x40u << 24;
    zmode = put(zmode, 1, PE_ZMODE_ENABLE_SHIFT, PE_ZMODE_ENABLE_SIZE);
    zmode = put(zmode, GX_GEQUAL, PE_ZMODE_FUNC_SHIFT, PE_ZMODE_FUNC_SIZE);
    zmode = put(zmode, 0, PE_ZMODE_MASK_SHIFT, PE_ZMODE_MASK_SIZE);
    GDWriteBPCmd(zmode);
    // CMODE0: blend on, src SRCALPHA, dst INVSRCALPHA, no logic op, dither on.
    u32 cmode0 = 0x41u << 24;
    cmode0 = put(cmode0, 1, PE_CMODE0_BLEND_ENABLE_SHIFT, PE_CMODE0_BLEND_ENABLE_SIZE);
    cmode0 = put(cmode0, GX_BL_SRCALPHA, PE_CMODE0_SFACTOR_SHIFT, PE_CMODE0_SFACTOR_SIZE);
    cmode0 = put(cmode0, GX_BL_INVSRCALPHA, PE_CMODE0_DFACTOR_SHIFT, PE_CMODE0_DFACTOR_SIZE);
    cmode0 = put(cmode0, 1, PE_CMODE0_DITHER_ENABLE_SHIFT, PE_CMODE0_DITHER_ENABLE_SIZE);
    cmode0 = put(cmode0, 1, PE_CMODE0_COLOR_MASK_SHIFT, PE_CMODE0_COLOR_MASK_SIZE);
    cmode0 = put(cmode0, 1, PE_CMODE0_ALPHA_MASK_SHIFT, PE_CMODE0_ALPHA_MASK_SIZE);
    GDWriteBPCmd(cmode0);
    // CMODE1: constant alpha 0x40, enabled.
    u32 cmode1 = 0x42u << 24;
    cmode1 = put(cmode1, 0x40, PE_CMODE1_CONSTANT_ALPHA_SHIFT, PE_CMODE1_CONSTANT_ALPHA_SIZE);
    cmode1 = put(cmode1, 1, PE_CMODE1_CONSTANT_ALPHA_ENABLE_SHIFT,
                 PE_CMODE1_CONSTANT_ALPHA_ENABLE_SIZE);
    GDWriteBPCmd(cmode1);
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    GxPeDebugState st;
    GXCompatDebugPeState(st);
    CHECK_EQ(st.cullMode, GX_CULL_BACK);
    CHECK_EQ(st.blendMode, GX_BM_BLEND);
    CHECK_EQ(st.blendSrc, GX_BL_SRCALPHA);
    CHECK_EQ(st.blendDst, GX_BL_INVSRCALPHA);
    CHECK_EQ(st.blendLogicOp, GX_LO_CLEAR);
    CHECK_EQ(st.zTest, 1);
    CHECK_EQ(st.zFunc, GX_GEQUAL);
    CHECK_EQ(st.zWrite, 0);
    CHECK_EQ(st.dither, 1);
    CHECK_EQ(st.colorUpdate, 1);
    CHECK_EQ(st.alphaUpdate, 1);
    CHECK_EQ(st.dstAlphaEnable, 1);
    CHECK_EQ(st.dstAlphaValue, 0x40);
}

TEST_CASE(gx_dl_tev_state_matches_api) {
    GXInit(nullptr, 0);

    // --- state set through the GX API --------------------------------------
    GXSetNumTexGens(2);
    GXSetNumTevStages(2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_RASC, GX_CC_ONE, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP1, GX_TEV_SWAP2);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    // Alpha env: the GXInit GXSetTevOp(REPLACE) preset (ZERO,ZERO,ZERO,TEXA)
    // must be overridden to the same APREV x4 the DL's TEVA register encodes.
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_APREV, GX_CA_APREV, GX_CA_APREV, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE,
                    GX_TEVPREV);
    GXSetTevColor(GX_TEVPREV, GXColor{0x12, 0x34, 0x56, 0x78});
    GXSetTevKColor(GX_KCOLOR0, GXColor{0xAB, 0xCD, 0xEF, 0x01});

    Platform::CompatGx::TevUboData uboApi;
    Platform::CompatGx::buildTevUbo(uboApi);

    // --- same state baked into a DL ----------------------------------------
    GXInit(nullptr, 0);  // reset the mirrors

    DlBuilder dl;
    dl.begin();
    // GEN_MODE: 2 texgens, 2 stages (field = n-1).
    GDWriteBPCmd(BP_GEN_MODE(2, 0, 1, 0, 0));
    // TREF (stage 0/1 pair): texmap 0, texcoord 0, chan 0, tex enable.
    u32 tref = 0x28u << 24;
    tref = put(tref, 0, RAS1_TREF_TI0_SHIFT, RAS1_TREF_TI0_SIZE);
    tref = put(tref, 0, RAS1_TREF_TC0_SHIFT, RAS1_TREF_TC0_SIZE);
    tref = put(tref, 1, RAS1_TREF_TE0_SHIFT, RAS1_TREF_TE0_SIZE);
    tref = put(tref, 0, RAS1_TREF_CC0_SHIFT, RAS1_TREF_CC0_SIZE);
    GDWriteBPCmd(tref);
    // TEVC stage 0: a=TEXC, b=RASC, c=ONE, d=CPREV, bias ADDHALF, add, clamp.
    u32 tevc = 0xC0u << 24;
    tevc = put(tevc, GX_CC_TEXC, TEV_COLOR_ENV_SELA_SHIFT, TEV_COLOR_ENV_SELA_SIZE);
    tevc = put(tevc, GX_CC_RASC, TEV_COLOR_ENV_SELB_SHIFT, TEV_COLOR_ENV_SELB_SIZE);
    tevc = put(tevc, GX_CC_ONE, TEV_COLOR_ENV_SELC_SHIFT, TEV_COLOR_ENV_SELC_SIZE);
    tevc = put(tevc, GX_CC_CPREV, TEV_COLOR_ENV_SELD_SHIFT, TEV_COLOR_ENV_SELD_SIZE);
    tevc = put(tevc, GX_TB_ADDHALF, TEV_COLOR_ENV_BIAS_SHIFT, TEV_COLOR_ENV_BIAS_SIZE);
    tevc = put(tevc, 0, TEV_COLOR_ENV_SUB_SHIFT, TEV_COLOR_ENV_SUB_SIZE);
    tevc = put(tevc, 1, TEV_COLOR_ENV_CLAMP_SHIFT, TEV_COLOR_ENV_CLAMP_SIZE);
    tevc = put(tevc, GX_CS_SCALE_1, TEV_COLOR_ENV_SHIFT_SHIFT, TEV_COLOR_ENV_SHIFT_SIZE);
    tevc = put(tevc, GX_TEVPREV, TEV_COLOR_ENV_DEST_SHIFT, TEV_COLOR_ENV_DEST_SIZE);
    GDWriteBPCmd(tevc);
    // TEVA stage 0: default a/b/c/d, ras sel SWAP1, tex sel SWAP2.
    u32 teva = 0xC1u << 24;
    teva = put(teva, GX_TEV_SWAP1, TEV_ALPHA_ENV_MODE_SHIFT, TEV_ALPHA_ENV_MODE_SIZE);
    teva = put(teva, GX_TEV_SWAP2, TEV_ALPHA_ENV_SWAP_SHIFT, TEV_ALPHA_ENV_SWAP_SIZE);
    teva = put(teva, GX_CA_APREV, TEV_ALPHA_ENV_SELA_SHIFT, TEV_ALPHA_ENV_SELA_SIZE);
    teva = put(teva, GX_CA_APREV, TEV_ALPHA_ENV_SELB_SHIFT, TEV_ALPHA_ENV_SELB_SIZE);
    teva = put(teva, GX_CA_APREV, TEV_ALPHA_ENV_SELC_SHIFT, TEV_ALPHA_ENV_SELC_SIZE);
    teva = put(teva, GX_CA_APREV, TEV_ALPHA_ENV_SELD_SHIFT, TEV_ALPHA_ENV_SELD_SIZE);
    teva = put(teva, GX_TB_ZERO, TEV_ALPHA_ENV_BIAS_SHIFT, TEV_ALPHA_ENV_BIAS_SIZE);
    teva = put(teva, 0, TEV_ALPHA_ENV_SUB_SHIFT, TEV_ALPHA_ENV_SUB_SIZE);
    teva = put(teva, 0, TEV_ALPHA_ENV_CLAMP_SHIFT, TEV_ALPHA_ENV_CLAMP_SIZE);
    teva = put(teva, GX_CS_SCALE_1, TEV_ALPHA_ENV_SHIFT_SHIFT, TEV_ALPHA_ENV_SHIFT_SIZE);
    teva = put(teva, GX_TEVPREV, TEV_ALPHA_ENV_DEST_SHIFT, TEV_ALPHA_ENV_DEST_SIZE);
    GDWriteBPCmd(teva);
    // KSEL (stage pair 0/1): K0/K0_A on stage 0, defaults (1/4, 1) on stage 1,
    // swap table 0 R/G = identity (0, 1).
    u32 ksel = 0xF6u << 24;
    ksel = put(ksel, 0, TEV_KSEL_XRB_SHIFT, TEV_KSEL_XRB_SIZE);
    ksel = put(ksel, 1, TEV_KSEL_XGA_SHIFT, TEV_KSEL_XGA_SIZE);
    ksel = put(ksel, GX_TEV_KCSEL_K0, TEV_KSEL_KCSEL0_SHIFT, TEV_KSEL_KCSEL0_SIZE);
    ksel = put(ksel, GX_TEV_KASEL_K0_A, TEV_KSEL_KASEL0_SHIFT, TEV_KSEL_KASEL0_SIZE);
    ksel = put(ksel, GX_TEV_KCSEL_1_4, TEV_KSEL_KCSEL1_SHIFT, TEV_KSEL_KCSEL1_SIZE);
    ksel = put(ksel, GX_TEV_KASEL_1, TEV_KSEL_KASEL1_SHIFT, TEV_KSEL_KASEL1_SIZE);
    GDWriteBPCmd(ksel);
    // TEV color register 0 (TEVPREV): R=0x12 A=0x78 / B=0x56 G=0x34 (11-bit).
    u32 regL = 0xE0u << 24;
    regL = put(regL, 0x12, TEV_REGISTERL_R_SHIFT, TEV_REGISTERL_R_SIZE);
    regL = put(regL, 0x78, TEV_REGISTERL_A_SHIFT, TEV_REGISTERL_A_SIZE);
    GDWriteBPCmd(regL);
    u32 regH = 0xE1u << 24;
    regH = put(regH, 0x56, TEV_REGISTERH_B_SHIFT, TEV_REGISTERH_B_SIZE);
    regH = put(regH, 0x34, TEV_REGISTERH_G_SHIFT, TEV_REGISTERH_G_SIZE);
    GDWriteBPCmd(regH);
    // TEV constant register K0: same RIDs as TEVPREV (0xE0/0xE1) but with
    // bit 23 set (the konst marker; PAD1 = 0x08 << 20) — the hardware
    // register file is selected by that bit. R=0xAB A=0x01 / B=0xEF G=0xCD.
    u32 kregL = 0xE0u << 24;
    kregL = put(kregL, 0xAB, TEV_KREGISTERL_R_SHIFT, TEV_KREGISTERL_R_SIZE);
    kregL = put(kregL, 0x01, TEV_KREGISTERL_A_SHIFT, TEV_KREGISTERL_A_SIZE);
    kregL |= 0x00800000u;  // konst marker
    GDWriteBPCmd(kregL);
    u32 kregH = 0xE1u << 24;
    // Konst registers are 8-bit: B at bits 0-7, G at bits 8-15 (the 11-bit
    // TEV_KREGISTERH_G_SHIFT=12 layout only applies to signed color regs).
    kregH = put(kregH, 0xEF, 0, 8);   // B
    kregH = put(kregH, 0xCD, 8, 8);   // G
    kregH |= 0x00800000u;
    GDWriteBPCmd(kregH);
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    Platform::CompatGx::TevUboData uboDl;
    Platform::CompatGx::buildTevUbo(uboDl);

    // The DL-applied state must produce the same UBO the API path does.
    CHECK_EQ(uboDl.header[0], uboApi.header[0]);
    CHECK_EQ(uboDl.header[1], uboApi.header[1]);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(uboDl.prev[i], uboApi.prev[i]);
        CHECK_EQ(uboDl.konst[0][i], uboApi.konst[0][i]);
    }
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(uboDl.stage[0].colorEnv[i], uboApi.stage[0].colorEnv[i]);
        CHECK_EQ(uboDl.stage[0].alphaEnv[i], uboApi.stage[0].alphaEnv[i]);
    }
    CHECK_EQ(uboDl.stage[0].opParams[0], uboApi.stage[0].opParams[0]);
    CHECK_EQ(uboDl.stage[0].opParams[1], uboApi.stage[0].opParams[1]);
    CHECK_EQ(uboDl.stage[0].opParams[2], uboApi.stage[0].opParams[2]);
    CHECK_EQ(uboDl.stage[0].opParams[3], uboApi.stage[0].opParams[3]);
    CHECK_EQ(uboDl.swapTables[0][0], uboApi.swapTables[0][0]);
    CHECK_EQ(uboDl.swapTables[0][1], uboApi.swapTables[0][1]);
}

TEST_CASE(gx_dl_xf_texgen) {
    GXInit(nullptr, 0);

    // The DL bakes the texgen registers the same way J3DGDSetTexCoordGen +
    // loadTexCoordGens do (XF_TEX0_ID + n, then XF_DUALTEX0_ID + n).
    DlBuilder dl;
    dl.begin();
    GDWriteXFCmd(XF_TEX0_ID, XF_TEX(0, 0, 0, XF_TEX0_INROW, 5, 0));   // GX_TG_TEX0, 2x4
    GDWriteXFCmd(XF_DUALTEX0_ID, XF_DUALTEX(0, 0));                   // mtx idx 0, no normalize
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    // resolveTexGen: passthrough TEX0 coord (no matrix). Build a vertex and
    // check the generator output via a draw.
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 1);
    GXPosition2f32(1.0f, 2.0f);
    GXTexCoord2f32(0.25f, 0.75f);
    GXEnd();

    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_EQ(count, 1);
    CHECK_EQ(stride, 4);  // pos(2) + tex(2)
    // Passthrough texgen: the vertex texcoord is unchanged.
    CHECK_NEAR(data[2], 0.25f, 1e-6f);
    CHECK_NEAR(data[3], 0.75f, 1e-6f);
}

// =============================================================================
// M5.7a: channel lighting (GXSetNumChans/GXSetChanCtrl/GXLoadLightObjImm +
// pos/nrm matrices) — headless. The evaluator (computeChannelLighting /
// applyChannelLighting) is Dolphin-exact; expected values below were derived
// by hand from the hardware formula:
//   ldir = normalize(lightPos - posView), attn per GX_AF_*,
//   lacc += round(diffuse * lightColor), clamp 0..255,
//   out = (mat * (lacc + (lacc >> 7))) >> 8.
// =============================================================================

TEST_CASE(gx_chan_lighting_basic) {
    // A single positional light (GX_AF_NONE, no attenuation), diffuse CLAMP.
    GXInit(nullptr, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0,
                  GX_DF_CLAMP, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{40, 50, 60, 200});
    GXSetChanMatColor(GX_COLOR0A0, GXColor{200, 180, 160, 255});

    GXLightObj light;
    GXInitLightPos(&light, 0.0f, 2.0f, 4.0f);
    GXInitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXInitLightColor(&light, GXColor{255, 128, 64, 255});
    GXLoadLightObjImm(&light, GX_LIGHT0);

    // Vertex at the origin, normal +Y (view space).
    // ldir = normalize(0,2,4) = (0, 0.4472136, 0.8944272); attn = 1;
    // ndl = 0.4472136; diffuse (CLAMP) = 0.4472136.
    // lacc = amb + round(0.4472136 * color):
    //   R: 40 + round(114.04) = 154; G: 50 + round(57.24) = 107;
    //   B: 60 + round(28.62) = 89.
    // out = (mat * (lacc + (lacc>>7))) >> 8:
    //   R: (200 * 155) >> 8 = 121; G: (180 * 107) >> 8 = 75; B: (160*89)>>8 = 55.
    // Alpha: amb a=200 + round(0.4472136*255)=114 -> 314 -> clamp 255,
    //   mat a=255 -> out = 255.
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out0[4], out1[4];
    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float nrm[3] = {0.0f, 1.0f, 0.0f};
    Platform::CompatGx::applyChannelLighting(pos, nrm, white, white, out0, out1);
    CHECK_NEAR(out0[0], 121.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[1], 75.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[2], 55.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[3], 1.0f, 1e-6f);
    // Channel 1 was not configured (GXInit defaults: disabled, SRC_VTX mat) ->
    // output = the vertex color.
    CHECK_NEAR(out1[0], 1.0f, 1e-6f);
    CHECK_NEAR(out1[1], 1.0f, 1e-6f);
    CHECK_NEAR(out1[2], 1.0f, 1e-6f);
    CHECK_NEAR(out1[3], 1.0f, 1e-6f);
}

TEST_CASE(gx_chan_lighting_spot) {
    // Spot light: GX_AF_SPOT with custom cos/dist attenuation coefficients.
    GXInit(nullptr, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0,
                  GX_DF_CLAMP, GX_AF_SPOT);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{30, 30, 30, 255});
    GXSetChanMatColor(GX_COLOR0A0, GXColor{255, 255, 255, 255});

    GXLightObj light;
    GXInitLightPos(&light, 0.0f, 2.0f, 0.0f);
    // Spot axis = -Y (GXInitLightDir negates its input, so the stored dir is
    // (0,1,0)). GXInitLightDir is not declared in this vendored GXLighting.h
    // (incomplete decompile), so write the struct field directly — that is
    // exactly what GXLoadLightObjImm reads.
    reinterpret_cast<GXLightObjInt*>(&light)->ldir[0] = 0.0f;
    reinterpret_cast<GXLightObjInt*>(&light)->ldir[1] = 1.0f;
    reinterpret_cast<GXLightObjInt*>(&light)->ldir[2] = 0.0f;
    GXInitLightAttn(&light, 2.0f, -1.0f, 0.5f, 1.0f, 0.5f, 0.0f);
    GXInitLightColor(&light, GXColor{200, 100, 50, 255});
    GXLoadLightObjImm(&light, GX_LIGHT0);

    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out0[4], out1[4];
    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float nrm[3] = {0.0f, 1.0f, 0.0f};
    Platform::CompatGx::applyChannelLighting(pos, nrm, white, white, out0, out1);
    // ldir = (0,2,0)/2 = (0,1,0); dist = 2; cosA = dot(ldir, L.dir) = 1;
    // cosAttn = max(0, 2 - 1 + 0.5) = 1.5; distAttn = 1 + 0.5*2 + 0 = 2;
    // attn = 0.75; ndl = 1 -> diffuse = 0.75.
    // lacc = amb + round(0.75 * color):
    //   R: 30 + 150 = 180; G: 30 + 75 = 105; B: 30 + 38 = 68.
    // out (mat 255): R 180, G (255*105)>>8 = 104, B (255*68)>>8 = 67.
    CHECK_NEAR(out0[0], 180.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[1], 104.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[2], 67.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[3], 1.0f, 1e-6f);
}

TEST_CASE(gx_chan_mirror_state) {
    GXInit(nullptr, 0);
    GXSetNumChans(2);
    // COLOR0A0 -> slots COLOR0(0) + ALPHA0(2).
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE,
                  GX_AF_NONE);
    // ALPHA1 with SPEC attenuation: the SDK forces the diffuse function to
    // GX_DF_NONE regardless of the requested GX_DF_SIGN.
    GXSetChanCtrl(GX_ALPHA1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
                  GX_LIGHT3 | GX_LIGHT5, GX_DF_SIGN, GX_AF_SPEC);
    GXSetChanMatColor(GX_COLOR0, GXColor{10, 20, 30, 40});
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{1, 2, 3, 4});

    Platform::CompatGx::GxChanDebugState st;
    Platform::CompatGx::getChanDebugState(st);
    CHECK_EQ(st.numChans, 2);
    // Slot 0 (COLOR0) + slot 2 (ALPHA0) from COLOR0A0.
    CHECK_EQ(st.enable[0], 0);
    CHECK_EQ(st.ambSrc[0], 1);
    CHECK_EQ(st.matSrc[0], 1);
    CHECK_EQ(st.enable[2], 0);
    CHECK_EQ(st.ambSrc[2], 1);
    // Slot 3 (ALPHA1): SPEC quirk -> diffFn = GX_DF_NONE (0), attnFn = SPEC (0).
    CHECK_EQ(st.enable[3], 1);
    CHECK_EQ(st.ambSrc[3], 0);
    CHECK_EQ(st.matSrc[3], 0);
    CHECK_EQ(static_cast<int>(st.lightMask[3]), 0x28);
    CHECK_EQ(st.diffFn[3], 0);
    CHECK_EQ(st.attnFn[3], 0);
    // Slot 1 (COLOR1) untouched -> GXInit default (disabled, SRC_VTX).
    CHECK_EQ(st.matSrc[1], 1);
    // Amb/mat colors: pair 0 rgb set; alpha of mat pair 0 unchanged (255).
    CHECK_EQ(st.ambColor[0][0], 1);
    CHECK_EQ(st.ambColor[0][1], 2);
    CHECK_EQ(st.ambColor[0][2], 3);
    CHECK_EQ(st.ambColor[0][3], 4);
    CHECK_EQ(st.matColor[0][0], 10);
    CHECK_EQ(st.matColor[0][1], 20);
    CHECK_EQ(st.matColor[0][2], 30);
    CHECK_EQ(st.matColor[0][3], 255);

    // Matrices: GX_PNMTX1 = pos slot 3, nrm slot 1 (id/3).
    GXSetCurrentMtx(GX_PNMTX1);
    Platform::CompatGx::getChanDebugState(st);
    CHECK_EQ(st.curPosMtx, 3);
    CHECK_EQ(st.curNrmMtx, 1);
    // Load a matrix into the GX_PNMTX5 slot (id 15 — the GX pos matrix ids
    // are 0,3,6,...27) and select it with GXSetCurrentMtx(GX_PNMTX5).
    f32 mtx[3][4] = {};
    mtx[0][0] = 1.0f; mtx[1][1] = 1.0f; mtx[2][2] = 1.0f;
    mtx[0][3] = 5.0f; mtx[1][3] = 6.0f; mtx[2][3] = 7.0f;
    GXLoadPosMtxImm(mtx, GX_PNMTX5);
    GXSetCurrentMtx(GX_PNMTX5);
    const float* pm = Platform::CompatGx::currentPosMtx();
    REQUIRE(pm != nullptr);
    CHECK_NEAR(pm[3], 5.0f, 1e-6f);
    CHECK_NEAR(pm[7], 6.0f, 1e-6f);
    CHECK_NEAR(pm[11], 7.0f, 1e-6f);
    // The normal matrix takes the 3x3 upper-left.
    GXLoadNrmMtxImm(mtx, 5);
    const float* nm = Platform::CompatGx::currentNrmMtx();
    REQUIRE(nm != nullptr);
    CHECK_NEAR(nm[0], 1.0f, 1e-6f);
    CHECK_NEAR(nm[4], 1.0f, 1e-6f);
    CHECK_NEAR(nm[8], 1.0f, 1e-6f);
}

TEST_CASE(gx_light_mvp_build) {
    // Row-vector GX chain: clip = posView * proj, posView = pos * posMtx.
    // buildMvp pushes the matrix the shader needs (column-vector multiply).
    // proj (rows-are-output): m00=2, m01=0.5, m11=2, m33=1.
    float proj[16] = {};
    proj[0] = 2.0f; proj[1] = 0.5f; proj[5] = 2.0f; proj[15] = 1.0f;
    // posMtx = translation (1,2,3), identity rotation (GX 3x4 rows-are-output).
    float pm[12] = {};
    pm[0] = pm[5] = pm[10] = 1.0f;
    pm[3] = 1.0f; pm[7] = 2.0f; pm[11] = 3.0f;

    float mvp[16];
    Platform::CompatGx::buildMvp(pm, proj, mvp);
    // mvp[r] = clip contribution row for output r: with the shader's
    // column-vector multiply gl_Position = mvp * pos, the upload is:
    //   [2,0,0,0, 0.5,2,0,0, 0,0,0,0, 3,4,0,1]
    const float expected[16] = {2, 0, 0, 0, 0.5f, 2, 0, 0,
                                0, 0, 0, 0, 3, 4, 0, 1};
    for (int i = 0; i < 16; ++i) {
        CHECK_NEAR(mvp[i], expected[i], 1e-6f);
    }
    // Identity pos matrix -> the pushed matrix is proj^T.
    float id[12] = {};
    id[0] = id[5] = id[10] = 1.0f;
    Platform::CompatGx::buildMvp(id, proj, mvp);
    CHECK_NEAR(mvp[0], 2.0f, 1e-6f);    // proj^T[0][0]
    CHECK_NEAR(mvp[1], 0.0f, 1e-6f);    // proj^T[0][1]
    CHECK_NEAR(mvp[4], 0.5f, 1e-6f);    // proj^T[1][0]
    CHECK_NEAR(mvp[5], 2.0f, 1e-6f);    // proj^T[1][1]
    CHECK_NEAR(mvp[15], 1.0f, 1e-6f);
}

// =============================================================================
// M5.7a DL routing: the interpreter applies the channel-lighting, matrix and
// light state when it arrives inside a display list (J3D bakes the material
// lighting into DLs: XF 0x1009/0x100A/0x100C/0x100E, pos/nrm matrices as
// XF 0x10 loads, lights as XF 0x600+ loads, CP MATINDEX_A via command 0x30,
// and the PCPU/NCPU pipelines as LOADINDX 0x20/0x28). Each test emits the
// exact GD byte stream the game writes and checks the mirror + evaluator.
// =============================================================================

// Packs a GXColor (r,g,b,a) into the raw XF u32 (R=MSB .. A=LSB).
u32 packRgba(u8 r, u8 g, u8 b, u8 a) {
    return (static_cast<u32>(r) << 24) | (static_cast<u32>(g) << 16) |
           (static_cast<u32>(b) << 8) | static_cast<u32>(a);
}

TEST_CASE(gx_dl_chan_lighting) {
    GXInit(nullptr, 0);

    DlBuilder dl;
    dl.begin();
    // The J3D baked format (J3DMatBlock.cpp): SETNUMCHAN (0x1009), amb
    // (0x100A/0x100B), mat (0x100C/0x100D) and the 4 channel controls
    // (0x100E..0x1011).
    GDWriteXFCmd(XF_NUMCOLORS_ID, 1);
    GDWriteXFCmd(XF_AMBIENT0_ID, packRgba(40, 50, 60, 200));
    GDWriteXFCmd(XF_MATERIAL0_ID, packRgba(200, 180, 160, 255));
    // chanCtrl (slot 0/2 = COLOR0A0): enable, amb REG, mat REG, light0,
    // diffFn CLAMP, attn NONE. SDK packing: bit0 mat, bit1 en, bit2 light0,
    // bit6 amb, bits7-8 diff, bit9 attnEn, bit10 attnSel (NONE = en0 sel1).
    const u32 chan = (1u << 1) | (1u << 2) | (2u << 7) | (1u << 10);
    for (int i = 0; i < 4; ++i) {
        GDWriteXFCmd(static_cast<u16>(XF_COLOR0CNTRL_ID + i), chan);
    }
    // Light 0 by DL: 16 XF regs at 0x600 (GXLoadLightObjImm's image) —
    // regs 0-2 zero, 3 = color, 4-6 = a (1,0,0), 7-9 = k (1,0,0),
    // 10-12 = pos (0,2,4), 13-15 = dir (0,0,0).
    GDWriteXFCmdHdr(0x600, 16);  // XF_LIGHT0 (no vendored macro)
    GDWrite_u32(0); GDWrite_u32(0); GDWrite_u32(0);
    GDWrite_u32(packRgba(255, 128, 64, 255));
    GDWrite_f32(1.0f); GDWrite_f32(0.0f); GDWrite_f32(0.0f);
    GDWrite_f32(1.0f); GDWrite_f32(0.0f); GDWrite_f32(0.0f);
    GDWrite_f32(0.0f); GDWrite_f32(2.0f); GDWrite_f32(4.0f);
    GDWrite_f32(0.0f); GDWrite_f32(0.0f); GDWrite_f32(0.0f);
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    Platform::CompatGx::GxChanDebugState st;
    Platform::CompatGx::getChanDebugState(st);
    CHECK_EQ(st.numChans, 1);
    // Slots 0..3 were all written by the 4-reg chanCtrl load.
    for (int s = 0; s < 4; ++s) {
        CHECK_EQ(st.enable[s], 1);
        CHECK_EQ(st.ambSrc[s], 0);
        CHECK_EQ(st.matSrc[s], 0);
        CHECK_EQ(static_cast<int>(st.lightMask[s]), 0x01);
        CHECK_EQ(st.diffFn[s], static_cast<int>(GX_DF_CLAMP));
        CHECK_EQ(st.attnFn[s], static_cast<int>(GX_AF_NONE));
    }
    CHECK_EQ(st.ambColor[0][0], 40);
    CHECK_EQ(st.ambColor[0][1], 50);
    CHECK_EQ(st.ambColor[0][2], 60);
    CHECK_EQ(st.ambColor[0][3], 200);
    CHECK_EQ(st.matColor[0][0], 200);
    CHECK_EQ(st.matColor[0][1], 180);
    CHECK_EQ(st.matColor[0][2], 160);
    CHECK_EQ(st.matColor[0][3], 255);

    // Same geometry as gx_chan_lighting_basic: the DL-fed light 0 is a
    // positional light at (0,2,4) with identity attenuation.
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out0[4], out1[4];
    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float nrm[3] = {0.0f, 1.0f, 0.0f};
    Platform::CompatGx::applyChannelLighting(pos, nrm, white, white, out0, out1);
    CHECK_NEAR(out0[0], 121.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[1], 75.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[2], 55.0f / 255.0f, 1e-6f);
    CHECK_NEAR(out0[3], 1.0f, 1e-6f);
}

TEST_CASE(gx_dl_matrices_and_matidx) {
    GXInit(nullptr, 0);

    DlBuilder dl;
    dl.begin();
    // J3DFifoLoadPosMtxImm(mtx, 0): XF 0x000, 12 regs.
    GDWriteXFCmdHdr(0x000, 12);
    for (int i = 0; i < 12; ++i) {
        GDWrite_f32(static_cast<float>(i) + 1.0f);  // 1..12 (row-major)
    }
    // J3DFifoLoadNrmMtxImm3x3(mtx, 0): XF 0x400, 9 regs.
    GDWriteXFCmdHdr(0x400, 9);
    for (int i = 0; i < 9; ++i) {
        GDWrite_f32(static_cast<float>(i) * 10.0f);  // 0,10,..,80
    }
    // GXSetCurrentMtx(GX_PNMTX0) = CP MATINDEX_A = 0x30, POSIDX bits 0-5.
    GDWriteCPCmd(0x30, GX_PNMTX0);
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    const float* pm = Platform::CompatGx::currentPosMtx();
    REQUIRE(pm != nullptr);
    for (int i = 0; i < 12; ++i) {
        CHECK_NEAR(pm[i], static_cast<float>(i) + 1.0f, 1e-6f);
    }
    const float* nm = Platform::CompatGx::currentNrmMtx();
    REQUIRE(nm != nullptr);
    for (int i = 0; i < 9; ++i) {
        CHECK_NEAR(nm[i], static_cast<float>(i) * 10.0f, 1e-6f);
    }

    // A second DL switches the current matrix to GX_PNMTX1 (pos slot 3,
    // nrm slot 1) and loads that slot through the imm path (the standard
    // J3D pipeline).
    DlBuilder dl2;
    dl2.begin();
    GDWriteXFCmdHdr(4 * 3, 12);  // pos matrix 3
    for (int i = 0; i < 12; ++i) {
        GDWrite_f32(100.0f + static_cast<float>(i));
    }
    GDWriteXFCmdHdr(0x400 + 3 * 3, 9);  // nrm matrix 3 (SDK GXLoadNrmMtxImm)
    for (int i = 0; i < 9; ++i) {
        GDWrite_f32(1000.0f + static_cast<float>(i) * 10.0f);
    }
    GDWriteCPCmd(0x30, GX_PNMTX1);
    const u32 size2 = dl2.end();

    GXCallDisplayList(dl2.buf, size2);

    Platform::CompatGx::GxChanDebugState st;
    Platform::CompatGx::getChanDebugState(st);
    CHECK_EQ(st.curPosMtx, 3);
    CHECK_EQ(st.curNrmMtx, 1);
    pm = Platform::CompatGx::currentPosMtx();
    CHECK_NEAR(pm[0], 100.0f, 1e-6f);
    CHECK_NEAR(pm[11], 111.0f, 1e-6f);
    nm = Platform::CompatGx::currentNrmMtx();
    CHECK_NEAR(nm[0], 1000.0f, 1e-6f);
    CHECK_NEAR(nm[8], 1080.0f, 1e-6f);
}

TEST_CASE(gx_dl_loadindx_matrices) {
    GXInit(nullptr, 0);

    // The PCPU/NCPU pipelines fetch the matrix data through LOADINDX from
    // the CP matrix arrays (GX_POS_MTX_ARRAY=21 / GX_NRM_MTX_ARRAY=22).
    float posArray[24];  // 2 matrices x 12 floats, stride 48
    float nrmArray[18];  // 2 matrices x 9 floats, stride 36
    for (int i = 0; i < 24; ++i) posArray[i] = 200.0f + static_cast<float>(i);
    for (int i = 0; i < 18; ++i) nrmArray[i] = 2000.0f + static_cast<float>(i) * 10.0f;
    GXSetArray(GX_POS_MTX_ARRAY, posArray, 48);
    GXSetArray(GX_NRM_MTX_ARRAY, nrmArray, 36);

    DlBuilder dl;
    dl.begin();
    // J3DSys::loadPosMtxIndx(1, 0): DL 0x20, payload u16 idx, u16
    // (0xB000 | slot*0x0C) — the u32 read back is (idx<<16)|addr.
    GDWrite_u8(0x20);
    GDWrite_u16(0);                 // index 0
    GDWrite_u16(0xB000 | (1 * 0x0C));  // -> XF addr 0x00C, count 12
    // J3DSys::loadNrmMtxIndx(1, 0): DL 0x28, payload u16 idx, u16
    // ((9-1)<<12) | (0x400 + 9*slot).
    GDWrite_u8(0x28);
    GDWrite_u16(0);                 // index 0
    GDWrite_u16((8 << 12) | (0x400 + 9 * 1));  // -> XF addr 0x409, count 9
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    // Matrix slot 1 (GX_PNMTX1 = id 3): pos = posArray[0..11], nrm =
    // nrmArray[0..8]. (J3DShapeMtx slot s maps to GX_PNMTX s.)
    const float* pm = Platform::CompatGx::posMtxAt(GX_PNMTX1);
    REQUIRE(pm != nullptr);
    for (int i = 0; i < 12; ++i) {
        CHECK_NEAR(pm[i], posArray[i], 1e-6f);
    }
    const float* nm = Platform::CompatGx::nrmMtxAt(GX_PNMTX1);
    REQUIRE(nm != nullptr);
    for (int i = 0; i < 9; ++i) {
        CHECK_NEAR(nm[i], nrmArray[i], 1e-6f);
    }
    // The previously-loaded matrix 0 is untouched.
    const float* pm0 = Platform::CompatGx::posMtxAt(GX_PNMTX0);
    CHECK_NEAR(pm0[0], 1.0f, 1e-6f);  // GXInit identity
    CHECK_NEAR(pm0[5], 1.0f, 1e-6f);
    CHECK_NEAR(pm0[10], 1.0f, 1e-6f);
}

// =============================================================================
// M5.7b: indirect (bump) texturing stages.
//
// Verifies (a) the six GX indirect APIs mirror the state, (b) the pure warp
// evaluator (evalIndirectWarp) reproduces the shader formula (lockstep), and
// (c) the display-list interpreter routes the BP indirect registers
// (GEN_MODE num-ind-stages, TEVIND 0x10+stage, IREF 0x24, IndTexScale 0x25,
// MTXA/B/C 0x06..0x08) into the same mirror, whose UBO packing is then
// checked. The reference case is the game's water materials (OceanBowl):
// ITF_8 + bias STU + matrix 0.1 * identity + ITW_OFF.
// =============================================================================

#include <revolution/gx/GXBump.h>

TEST_CASE(gx_ind_mirror_state) {
    GXInit(nullptr, 0);
    GXSetNumIndStages(1);
    GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD2, GX_TEXMAP2);
    GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_2, GX_ITS_1);
    const f32 indmtx[2][3] = {{0.1f, 0.0f, 0.0f}, {0.0f, 0.1f, 0.0f}};
    GXSetIndTexMtx(GX_ITM_0, indmtx, 0);
    GXSetTevIndWarp(GX_TEVSTAGE3, GX_INDTEXSTAGE0, GX_TRUE, GX_FALSE, GX_ITM_0);

    Platform::CompatGx::GxIndDebugState st;
    Platform::CompatGx::getIndDebugState(st);
    CHECK_EQ(st.numStages, 1);
    CHECK_EQ(st.texCoord[0], GX_TEXCOORD2);
    CHECK_EQ(st.texMap[0], GX_TEXMAP2);
    CHECK_EQ(st.scaleS[0], GX_ITS_2);
    CHECK_EQ(st.scaleT[0], GX_ITS_1);
    // GXSetTevIndWarp(signed=1, replace=0, ITM_0) -> ITF_8, bias STU,
    // matrix ITM_0, wraps OFF.
    CHECK_EQ(st.fmt[3], GX_ITF_8);
    CHECK_EQ(st.bias[3], GX_ITB_STU);
    CHECK_EQ(st.mtxSel[3], GX_ITM_0);
    CHECK_EQ(st.wrapS[3], GX_ITW_OFF);
    CHECK_EQ(st.wrapT[3], GX_ITW_OFF);
    CHECK_EQ(st.addPrev[3], 0);
    // Matrix slot 0 (GX_ITM_0 -> slot 0) keeps the float values and exp 0.
    CHECK_NEAR(st.mtx[0][0][0], 0.1f, 1e-6f);
    CHECK_NEAR(st.mtx[0][1][1], 0.1f, 1e-6f);
    CHECK_EQ(st.mtxScaleExp[0], 0);
}

TEST_CASE(gx_ind_warp_evaluator) {
    // Water case: base UV (0.5, 0.5), indirect texel components (1, 0.5, 0),
    // matrix 0.1*identity, exp 0, direct map 640x448, bias STU, wraps OFF.
    //   v = (2*1-1, 2*0.5-1, 2*0-1) = (1, 0, -1)
    //   offsetTexel = (0.1*1, 0.1*0) * 128 = (12.8, 0)
    //   offsetUv = (12.8/640, 0/448) = (0.02, 0)
    //   out = uv + offset = (0.52, 0.5)
    const float uv[2] = {0.5f, 0.5f};
    const float comp[3] = {1.0f, 0.5f, 0.0f};
    const float mtx[2][3] = {{0.1f, 0.0f, 0.0f}, {0.0f, 0.1f, 0.0f}};
    const float texDim[2] = {640.0f, 448.0f};
    float outUv[2];
    Platform::CompatGx::evalIndirectWarp(uv, comp, mtx, 0, texDim, GX_ITB_STU,
                                         GX_ITW_OFF, GX_ITW_OFF, outUv);
    CHECK_NEAR(outUv[0], 0.52f, 1e-5f);
    CHECK_NEAR(outUv[1], 0.5f, 1e-5f);

    // Bias NONE: components stay 0..1, so v = comp directly.
    const float comp2[3] = {0.25f, 0.5f, 0.75f};
    Platform::CompatGx::evalIndirectWarp(uv, comp2, mtx, 0, texDim, GX_ITB_NONE,
                                         GX_ITW_OFF, GX_ITW_OFF, outUv);
    // offsetTexel = (0.1*0.25, 0.1*0.5)*128 = (3.2, 6.4) -> UV (0.005, 0.01428)
    CHECK_NEAR(outUv[0], 0.505f, 1e-5f);
    CHECK_NEAR(outUv[1], 0.514285f, 1e-5f);

    // Wrap 0 (GX_ITW_0): the base coord becomes (0,0), offset applies only.
    Platform::CompatGx::evalIndirectWarp(uv, comp, mtx, 0, texDim, GX_ITB_STU,
                                         GX_ITW_0, GX_ITW_0, outUv);
    CHECK_NEAR(outUv[0], 0.02f, 1e-5f);
    CHECK_NEAR(outUv[1], 0.0f, 1e-5f);

    // Negative exponent: 2^scaleExp = 2^-1 -> offset halves (6.4 texels).
    Platform::CompatGx::evalIndirectWarp(uv, comp, mtx, -1, texDim, GX_ITB_STU,
                                         GX_ITW_OFF, GX_ITW_OFF, outUv);
    CHECK_NEAR(outUv[0], 0.51f, 1e-5f);
}

TEST_CASE(gx_dl_ind_stages) {
    GXInit(nullptr, 0);

    DlBuilder dl;
    dl.begin();
    // GEN_MODE: 2 texgens, 3 tev stages, 1 ind stage (J3DGDSetGenMode image).
    GDWriteBPCmd((0x00u << 24) | (2u) | (1u << 6) | (2u << 10));
    // TEVIND stage 3 (RID 0x13): BT=0, FMT=0 (ITF_8), BIAS=7 (STU), BS=0,
    // M=1 (ITM_0), SW=0, TW=0, LB=0, FB=0 (GXBump.c packing).
    GDWriteBPCmd((0x13u << 24) | (7u << 4) | (1u << 9));
    // IREF (0x24): BI0 = TEXMAP2, BC0 = TEXCOORD2.
    GDWriteBPCmd((0x24u << 24) | (2u) | (2u << 16));
    // IndTexScale0 (0x25): SS0/TS0 = GX_ITS_1 (0), SS1/TS1 = 0.
    GDWriteBPCmd(0x25u << 24);
    // MTXA (0x06): MA = 0.1*1024 = 102, MB = 0, S = (0+0x11)&3 = 1.
    GDWriteBPCmd((0x06u << 24) | 102u | (1u << 22));
    // MTXB (0x07): MC = 0, MD = 102, S = (0x11>>2)&3 = 0.
    GDWriteBPCmd((0x07u << 24) | (102u << 11));
    // MTXC (0x08): ME = 0, MF = 0, S = (0x11>>4)&3 = 1.
    GDWriteBPCmd((0x08u << 24) | (1u << 22));
    const u32 size = dl.end();

    GXCallDisplayList(dl.buf, size);

    Platform::CompatGx::GxIndDebugState st;
    Platform::CompatGx::getIndDebugState(st);
    CHECK_EQ(st.numStages, 1);
    CHECK_EQ(st.texCoord[0], GX_TEXCOORD2);
    CHECK_EQ(st.texMap[0], GX_TEXMAP2);
    CHECK_EQ(st.scaleS[0], GX_ITS_1);
    CHECK_EQ(st.scaleT[0], GX_ITS_1);
    CHECK_EQ(st.fmt[3], GX_ITF_8);
    CHECK_EQ(st.bias[3], GX_ITB_STU);
    CHECK_EQ(st.mtxSel[3], GX_ITM_0);
    CHECK_NEAR(st.mtx[0][0][0], 0.1f, 1e-3f);  // 102/1024
    CHECK_NEAR(st.mtx[0][1][1], 0.1f, 1e-3f);
    CHECK_EQ(st.mtxScaleExp[0], 0);

    // UBO packing: header.z = 1 ind stage; indParams[0].x = coord|map<<8;
    // indMtx rows carry the matrix and the 128/2^0 = 128 calibration; the
    // stage-3 tevind pack lands in stage[3].reserved[0].
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    CHECK_EQ(ubo.header[2], 1);
    CHECK_EQ(ubo.indParams[0][0], 2 | (2 << 8));
    CHECK_NEAR(ubo.indMtx[0][0][0], 0.1f, 1e-3f);
    CHECK_NEAR(ubo.indMtx[0][0][3], 128.0f, 1e-3f);
    CHECK_NEAR(ubo.indMtx[0][1][1], 0.1f, 1e-3f);
    CHECK_NEAR(ubo.indMtx[0][1][3], 128.0f, 1e-3f);
    CHECK_EQ(ubo.stage[3].reserved[0], (7 << 8) | (1 << 12));
}
