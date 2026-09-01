// =============================================================================
// M5.7c — EFB copy tests (docs/gx.md §J).
//
// Two groups:
//   * Pure CPU tests (no GPU): the copy-state mirror, the SDK size/scale
//     formulas, and the GX tiled encoders (round-trip through the Bti
//     decoder, which is what GXLoadTexObj will do with a GXCopyTex buffer).
//   * GPU tests (skip without a display/Vulkan, run under Xvfb): the full
//     console-like frame — render the scene into the EFB render target,
//     GXCopyDisp blits it to the swapchain, and GXCopyTex reads the EFB back
//     into a texture buffer the game can load with GXInitTexObj.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/gx/Bti.h"
#include "compat/gx/GXCompat.h"
#include "platform/Renderer/Renderer.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

namespace {

// Test image used by the encode round-trip: 64x64 with a 4x4 checker of
// distinct colors + varying alpha so every format's quantization shows.
void makePattern(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const int cell = ((x / 8) + (y / 8)) % 2;
            uint8_t* p = &rgba[(y * w + x) * 4];
            switch (cell) {
                case 0: p[0] = 255; p[1] = 0; p[2] = 0; break;
                default: p[0] = 0; p[1] = 0; p[2] = 255; break;
            }
            p[3] = 255; // opaque: the alpha-less GX formats drop alpha
        }
    }
}

// Decodes back to RGBA8 through the BTI path and compares with tolerance.
bool roundTrip(uint8_t gxFmt, bool mipmap, uint32_t w, uint32_t h, int tol) {
    std::vector<uint8_t> rgba;
    makePattern(rgba, w, h);
    const size_t need = Platform::CompatGx::encodedTexSize(w, h, gxFmt, mipmap);
    std::vector<uint8_t> enc(need, 0);
    if (!Platform::CompatGx::encodeEfbRgbaToGx(rgba.data(), w, h, gxFmt, mipmap,
                                               enc.data(), enc.size())) {
        return false;
    }
    // Decode only the base level (what GXInitTexObj/GXLoadTexObj consumes).
    const size_t base = Platform::CompatGx::encodedTexSize(w, h, gxFmt, false);
    std::vector<uint8_t> dec(static_cast<size_t>(w) * h * 4, 0);
    if (!Platform::CompatGx::btiDecodeToRgba8(enc.data(), base, w, h, gxFmt, nullptr, 0, 0,
                          dec.data())) {
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* a = &rgba[(y * w + x) * 4];
            const uint8_t* b = &dec[(y * w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                if (std::abs(int(a[c]) - int(b[c])) > tol) {
                    return false;
                }
            }
        }
    }
    return true;
}

// Round-trip with a uniform alpha value (for the alpha-capable formats).
bool roundTripAlpha(uint8_t gxFmt, uint8_t alpha, int tol) {
    const uint32_t w = 32, h = 32;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        // Gray color: the IA formats store intensity only.
        rgba[i] = 100;
        rgba[i + 1] = 100;
        rgba[i + 2] = 100;
        rgba[i + 3] = alpha;
    }
    const size_t need = Platform::CompatGx::encodedTexSize(w, h, gxFmt, false);
    std::vector<uint8_t> enc(need, 0);
    if (!Platform::CompatGx::encodeEfbRgbaToGx(rgba.data(), w, h, gxFmt, false,
                                               enc.data(), enc.size())) {
        return false;
    }
    std::vector<uint8_t> dec(static_cast<size_t>(w) * h * 4, 0);
    if (!Platform::CompatGx::btiDecodeToRgba8(enc.data(), need, w, h, gxFmt,
                                              nullptr, 0, 0, dec.data())) {
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* a = &rgba[(y * w + x) * 4];
            const uint8_t* b = &dec[(y * w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                if (std::abs(int(a[c]) - int(b[c])) > tol) {
                    return false;
                }
            }
        }
    }
    return true;
}

// Round-trip with a grayscale input (the I4/I8/IA4/IA8 formats store
// intensity only — a colored input cannot round-trip).
bool roundTripGray(uint8_t gxFmt, int tol) {
    const uint32_t w = 64, h = 64;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t g = static_cast<uint8_t>((x * 255) / (w - 1));
            uint8_t* p = &rgba[(y * w + x) * 4];
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
        }
    }
    const size_t need = Platform::CompatGx::encodedTexSize(w, h, gxFmt, false);
    std::vector<uint8_t> enc(need, 0);
    if (!Platform::CompatGx::encodeEfbRgbaToGx(rgba.data(), w, h, gxFmt, false,
                                               enc.data(), enc.size())) {
        return false;
    }
    std::vector<uint8_t> dec(static_cast<size_t>(w) * h * 4, 0);
    if (!Platform::CompatGx::btiDecodeToRgba8(enc.data(), need, w, h, gxFmt,
                                              nullptr, 0, 0, dec.data())) {
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* a = &rgba[(y * w + x) * 4];
            const uint8_t* b = &dec[(y * w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                if (std::abs(int(a[c]) - int(b[c])) > tol) {
                    return false;
                }
            }
        }
    }
    return true;
}

// GX draw setup shared by the GPU tests: full-screen quad in NDC with a
// solid vertex color (TEV PASSCLR -> output = vertex color).
const float kPos[4][3] = {{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
                          {1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}};
const uint8_t kRed[4][4] = {{255, 0, 0, 255}, {255, 0, 0, 255},
                            {255, 0, 0, 255}, {255, 0, 0, 255}};

void setupGxDraw() {
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetArray(GX_VA_POS, kPos, sizeof(kPos[0]));
    GXSetArray(GX_VA_CLR0, kRed, sizeof(kRed[0]));
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetNumTevStages(1);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

void drawFullScreenQuad() {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; ++i) {
        GXPosition1x16(static_cast<u16>(i));
        GXColor1x8(static_cast<u8>(i));
    }
    GXEnd();
}

// Decodes a GXCopyTex buffer (RGBA8, no mips) and checks one pixel.
bool decodeCopyAndCheck(const uint8_t* enc, uint32_t w, uint32_t h, uint8_t fmt,
                        int x, int y, int r, int g, int b, int a, int tol) {
    std::vector<uint8_t> dec(static_cast<size_t>(w) * h * 4, 0);
    const size_t base = Platform::CompatGx::encodedTexSize(w, h, fmt, false);
    if (!Platform::CompatGx::btiDecodeToRgba8(enc, base, w, h, fmt, nullptr, 0, 0, dec.data())) {
        return false;
    }
    const uint8_t* p = &dec[(static_cast<size_t>(y) * w + x) * 4];
    return std::abs(int(p[0]) - r) <= tol && std::abs(int(p[1]) - g) <= tol &&
           std::abs(int(p[2]) - b) <= tol && std::abs(int(p[3]) - a) <= tol;
}

} // namespace

// --- pure CPU: state mirror --------------------------------------------------

TEST_CASE(gx_copy_state_mirror) {
    GXSetDispCopySrc(0, 0, 640, 448);
    GXSetDispCopyDst(640, 448);
    GXSetTexCopySrc(16, 32, 128, 64);
    GXSetTexCopyDst(128, 64, GX_TF_RGB565, GX_FALSE);
    GXSetCopyClear(GXColor{10, 20, 30, 40}, 0xABCDEF);
    GXSetCopyClamp(GX_CLAMP_BOTTOM);
    GXSetCopyFilter(GX_TRUE, nullptr, GX_FALSE, nullptr);
    GXSetDispCopyGamma(GX_GM_2_2);
    GXSetDispCopyFrame2Field(GX_COPY_INTLC_EVEN);
    GXSetDispCopyYScale(1.0f);

    GxCopyDebugState st;
    GXCompatDebugCopyState(st);
    CHECK_EQ(st.dispSrcX, 0);
    CHECK_EQ(st.dispSrcY, 0);
    CHECK_EQ(st.dispSrcW, 640);
    CHECK_EQ(st.dispSrcH, 448);
    CHECK_EQ(st.dispDstW, 640);
    CHECK_EQ(st.dispDstH, 448);
    CHECK_EQ(st.texSrcX, 16);
    CHECK_EQ(st.texSrcY, 32);
    CHECK_EQ(st.texSrcW, 128);
    CHECK_EQ(st.texSrcH, 64);
    CHECK_EQ(st.texDstW, 128);
    CHECK_EQ(st.texDstH, 64);
    CHECK_EQ(st.texFmt, static_cast<int>(GX_TF_RGB565));
    CHECK_EQ(st.texMipmap, static_cast<int>(GX_FALSE));
    CHECK_EQ(st.copyClearR, 10);
    CHECK_EQ(st.copyClearG, 20);
    CHECK_EQ(st.copyClearB, 30);
    CHECK_EQ(st.copyClearA, 40);
    CHECK(st.copyClearZ == 0xABCDEF);
    CHECK_EQ(st.copyClamp, static_cast<int>(GX_CLAMP_BOTTOM));
    CHECK_EQ(st.copyFilterAA, static_cast<int>(GX_TRUE));
    CHECK_EQ(st.copyFilterVFilter, static_cast<int>(GX_FALSE));
    CHECK_EQ(st.dispCopyGamma, static_cast<int>(GX_GM_2_2));
    CHECK_EQ(st.dispCopyFrame2Field, static_cast<int>(GX_COPY_INTLC_EVEN));
    CHECK_EQ(st.dispCopyYScale, 256u); // 1.0f * 256
    // The EFB target follows the disp-copy source size (created lazily).
    CHECK_EQ(st.efbW, 0u);
    CHECK_EQ(st.efbH, 0u);
}

// --- pure CPU: SDK formulas ---------------------------------------------------

TEST_CASE(gx_copy_sdk_formulas) {
    // GXGetYScaleFactor: 256 * xfbHeight / (2 * efbHeight).
    // 480p (efb 448): 256*480/896 = 137.14...
    const f32 yscale = GXGetYScaleFactor(448, 480);
    CHECK_NEAR(yscale, 137.142857f, 0.01f);
    // GXGetNumXfbLines back-calculates the XFB height: 896 * 137.14 / 256.
    CHECK_EQ(GXGetNumXfbLines(448, yscale), 480u);

    // PAL 576-line mode (efb 528, xfb 574): 256*574/1056 = 139.15.
    const f32 y2 = GXGetYScaleFactor(528, 574);
    CHECK_NEAR(y2, 139.15f, 0.1f);
    CHECK_EQ(GXGetNumXfbLines(528, y2), 574u);

    // GXSetDispCopyYScale register value = yScale * 256 (truncated).
    CHECK_EQ(GXSetDispCopyYScale(yscale), 35108u); // floor(137.142857*256)

    // GXGetTexBufferSize: bytes per format (JUTTexture sizes its buffers).
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_I4, GX_FALSE, 1), 2048u);
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_I8, GX_FALSE, 1), 4096u);
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_IA8, GX_FALSE, 1), 8192u);
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_RGB565, GX_FALSE, 1), 8192u);
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_RGBA8, GX_FALSE, 1), 16384u);
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_CMPR, GX_FALSE, 1), 2048u);
    // Mipmap chain (maxLod levels 1..8, w/h floored at 1):
    //   base 16384 + 4096 + 1024 + 256 + 64 + 16 + 4 + 4 + 4 = 21852
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_RGBA8, GX_TRUE, 8), 21852u);
    // With maxLod 6 the chain ends at 2x2: 16384+4096+1024+256+64+16+4 = 21844.
    CHECK_EQ(GXGetTexBufferSize(64, 64, GX_TF_RGBA8, GX_TRUE, 6), 21844u);
    // Unknown format -> 0.
    CHECK_EQ(GXGetTexBufferSize(64, 64, 0x77, GX_FALSE, 1), 0u);
}

// --- pure CPU: GX tiled encoders (round-trip through the Bti decoder) ---------

TEST_CASE(gx_copy_encode_roundtrip) {
    // Lossless-ish formats: tight tolerance.
    CHECK(roundTrip(GX_TF_RGBA8, false, 64, 64, 0));
    CHECK(roundTrip(GX_TF_RGBA8, false, 640, 448, 0));
    // RGB565 loses the low bits.
    CHECK(roundTrip(GX_TF_RGB565, false, 64, 64, 8));
    CHECK(roundTrip(GX_TF_RGB5A3, false, 64, 64, 8));
    // Grayscale formats: input is gray, tolerance for the nibble/rounding.
    CHECK(roundTripGray(GX_TF_I8, 1));
    CHECK(roundTripGray(GX_TF_IA8, 1));
    CHECK(roundTripGray(GX_TF_IA4, 10));
    CHECK(roundTripGray(GX_TF_I4, 10));
    // CMPR is lossy (block endpoints); loose tolerance.
    CHECK(roundTrip(GX_TF_CMPR, false, 64, 64, 60));
    CHECK(roundTrip(GX_TF_CMPR, false, 128, 64, 60));

    // Mipmap chains: base level must still round-trip.
    CHECK(roundTrip(GX_TF_RGBA8, true, 64, 64, 0));
    CHECK(roundTrip(GX_TF_RGB565, true, 64, 64, 8));

    // Alpha-capable formats round-trip a uniform alpha (RGBA8/IA8 exact,
    // IA4 4-bit, RGB5A3 3-bit).
    CHECK(roundTripAlpha(GX_TF_RGBA8, 128, 0));
    CHECK(roundTripAlpha(GX_TF_IA8, 128, 0));
    CHECK(roundTripAlpha(GX_TF_IA4, 128, 12));
    CHECK(roundTripAlpha(GX_TF_RGB5A3, 128, 24));

    // Format validation: unsupported sizes/formats are rejected.
    std::vector<uint8_t> rgba(16 * 4 * 4, 0);
    std::vector<uint8_t> out(256, 0);
    CHECK(!Platform::CompatGx::encodeEfbRgbaToGx(rgba.data(), 3, 4, GX_TF_RGBA8,
                                                 false, out.data(), out.size()));
    CHECK(!Platform::CompatGx::encodeEfbRgbaToGx(rgba.data(), 64, 64, 0x77,
                                                 false, out.data(), out.size()));
    CHECK_EQ(Platform::CompatGx::encodedTexSize(64, 64, 0x77, false), 0u);
    // CMPR requires width % 8.
    CHECK_EQ(Platform::CompatGx::encodedTexSize(64, 64, GX_TF_CMPR, false),
             Platform::CompatGx::btiImageSize(64, 64, GX_TF_CMPR));
}

// --- GPU: EFB present + GXCopyTex (runs under Xvfb; skips without video) -----

TEST_CASE(gx_copy_efb_present_and_readback) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SKIP("SDL_Init failed (no video subsystem)");
        return;
    }
    SDL_Window* window = SDL_CreateWindow("galaxy-pc-test-gxcopy", 128, 128,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!window) {
        SDL_Quit();
        SKIP("SDL_CreateWindow (hidden, Vulkan) failed");
        return;
    }
    Platform::RendererConfig cfg{};
    cfg.appName = "galaxy-pc-tests";
    cfg.enableValidation = true; // catch layout/blit mistakes
    cfg.vsync = false;
    if (!Platform::Renderer::init(window, cfg)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        SKIP("Platform::Renderer init failed (no Vulkan surface/ICD)");
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    REQUIRE(r.isInitialized());

    setupGxDraw();
    GXSetViewport(0.0f, 0.0f, 640.0f, 448.0f, 0.0f, 1.0f);
    // The EFB pass clears with this (GXSetCopyClear == the EFB clear).
    GXSetCopyClear(GXColor{40, 60, 80, 255}, 0xFFFFFF);

    // Frame A: render the red full-screen quad into the EFB, present.
    REQUIRE(r.beginFrame());
    GXClear(GX_CLEAR_COLOR);
    r.beginPass(static_cast<Platform::RenderTargetHandle>(
        Platform::CompatGx::getEfbRenderTarget()));
    REQUIRE(r.inPass());
    drawFullScreenQuad();
    r.endPass();
    GXCopyDisp(nullptr, GX_TRUE); // blit EFB -> swapchain
    r.endFrame();
    GXCompatEndFrame();

    // Frame B: before clearing the EFB, GXCopyTex captures the previous
    // frame's content (the red quad) into a texture buffer.
    REQUIRE(r.beginFrame());
    {
        GXSetTexCopySrc(0, 0, 64, 64);
        GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
        const size_t need = Platform::CompatGx::encodedTexSize(64, 64, GX_TF_RGBA8,
                                                               false);
        std::vector<uint8_t> buf(need, 0);
        GXCopyTex(buf.data(), GX_FALSE);
        // Red quad covers the whole EFB -> the copy is red (opaque).
        CHECK(decodeCopyAndCheck(buf.data(), 64, 64, GX_TF_RGBA8, 8, 8,
                                 255, 0, 0, 255, 8));
        CHECK(decodeCopyAndCheck(buf.data(), 64, 64, GX_TF_RGBA8, 56, 56,
                                 255, 0, 0, 255, 8));
    }
    // Finish frame B (clears the EFB with the copy-clear color, presents).
    GXClear(GX_CLEAR_COLOR);
    r.beginPass(static_cast<Platform::RenderTargetHandle>(
        Platform::CompatGx::getEfbRenderTarget()));
    drawFullScreenQuad();
    r.endPass();
    GXCopyDisp(nullptr, GX_TRUE);
    r.endFrame();
    GXCompatEndFrame();

    // Frame C: an EFB with nothing drawn shows the GXSetCopyClear color.
    REQUIRE(r.beginFrame());
    {
        GXSetTexCopySrc(0, 0, 32, 32);
        GXSetTexCopyDst(32, 32, GX_TF_RGBA8, GX_FALSE);
        r.beginPass(static_cast<Platform::RenderTargetHandle>(
            Platform::CompatGx::getEfbRenderTarget())); // clears with (40,60,80)
        r.endPass();
        const size_t need = Platform::CompatGx::encodedTexSize(32, 32, GX_TF_RGBA8,
                                                               false);
        std::vector<uint8_t> buf(need, 0);
        GXCopyTex(buf.data(), GX_FALSE);
        CHECK(decodeCopyAndCheck(buf.data(), 32, 32, GX_TF_RGBA8, 4, 4,
                                 40, 60, 80, 255, 2));
    }
    GXCopyDisp(nullptr, GX_TRUE); // no pass blitted this frame: swapchain pass
    r.endFrame();
    GXCompatEndFrame();

    // Readback validation outside the frame loop: readRenderTarget directly.
    REQUIRE(r.beginFrame());
    {
        r.beginPass(static_cast<Platform::RenderTargetHandle>(
            Platform::CompatGx::getEfbRenderTarget()));
        drawFullScreenQuad();
        r.endPass();
        r.flushFrame(); // submit so the readback below sees the red quad
        std::vector<uint8_t> rgba(64 * 64 * 4, 0);
        CHECK(r.readRenderTarget(
            static_cast<Platform::RenderTargetHandle>(
                Platform::CompatGx::getEfbRenderTarget()),
            0, 0, 64, 64, rgba.data()));
        const uint8_t* p = &rgba[(32 * 64 + 32) * 4];
        CHECK(std::abs(int(p[0]) - 255) <= 8);
        CHECK(std::abs(int(p[1]) - 0) <= 8);
        CHECK(std::abs(int(p[2]) - 0) <= 8);
    }
    GXCopyDisp(nullptr, GX_TRUE);
    r.endFrame();
    GXCompatEndFrame();

    GXCompatShutdown();
    Platform::Renderer::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
}
