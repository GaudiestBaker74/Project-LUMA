// =============================================================================
// TEMPORARY local benchmark (NOT shipped): planets-like vertex throughput.
//
// Pushes 64 strips x 692 verts = 44,288 verts/frame through the real GX
// capture path (INDEX16 attributes + 2 lights + 2 texgens + back-face culling
// — the shape of the fileselect planet draws) and reports ms/frame, so the
// RelWithDebInfo and Debug builds can be compared directly against the user's
// log numbers (planets: 44,247 verts/frame, cpu-render 108-222 ms).
// =============================================================================

#include "tests/test_runner.h"

#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"

#include "compat/gx/GXCompat.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kStrips = 64;
constexpr int kVertsPerStrip = 692;

}  // namespace

TEST_CASE(bench_gx_planets_like_frame_cost) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SKIP("SDL_Init failed (no video subsystem)");
        return;
    }
    SDL_Window* window = SDL_CreateWindow("galaxy-pc-bench", 128, 128,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!window) {
        SDL_Quit();
        SKIP("SDL_CreateWindow (hidden, Vulkan) failed");
        return;
    }
    Platform::RendererConfig cfg{};
    cfg.appName = "galaxy-pc-tests";
    cfg.enableValidation = false;
    cfg.vsync = false;
    if (!Platform::Renderer::init(window, cfg)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        SKIP("Platform::Renderer init failed (no Vulkan surface/ICD)");
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    REQUIRE(r.isInitialized());

    GXInit(nullptr, 0);

    // Attribute arrays (planet-like: sphere positions/normals, gradient color,
    // uv). INDEX16 references into them, exactly like the J3D planet packets.
    static float sPos[kVertsPerStrip][3];
    static float sNrm[kVertsPerStrip][3];
    static unsigned char sClr[kVertsPerStrip][4];
    static float sTex[kVertsPerStrip][2];
    for (int v = 0; v < kVertsPerStrip; ++v) {
        const float t = static_cast<float>(v) / static_cast<float>(kVertsPerStrip);
        const float lat = t * 3.14159265f;
        const float lon = t * 6.2831853f * 8.0f;
        const float sy = std::sin(lat);
        sNrm[v][0] = sy * std::cos(lon);
        sNrm[v][1] = std::cos(lat);
        sNrm[v][2] = sy * std::sin(lon);
        sPos[v][0] = sNrm[v][0] * 100.0f;
        sPos[v][1] = sNrm[v][1] * 100.0f;
        sPos[v][2] = sNrm[v][2] * 100.0f;
        sClr[v][0] = static_cast<unsigned char>(v & 0xFF);
        sClr[v][1] = static_cast<unsigned char>((v * 3) & 0xFF);
        sClr[v][2] = static_cast<unsigned char>((v * 7) & 0xFF);
        sClr[v][3] = 255;
        sTex[v][0] = t * 8.0f;
        sTex[v][1] = 1.0f - t;
    }
    GXSetArray(GX_VA_POS, sPos, sizeof(sPos[0]));
    GXSetArray(GX_VA_NRM, sNrm, sizeof(sNrm[0]));
    GXSetArray(GX_VA_CLR0, sClr, sizeof(sClr[0]));
    GXSetArray(GX_VA_TEX0, sTex, sizeof(sTex[0]));

    // One-shot planet-like state (per-frame in the game it comes from the J3D
    // material; here it is set once — the per-frame cost measured below is the
    // vertex pipeline, which is what the heartbeat blames).
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXSetNumChans(2);
    GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
                  GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_SPOT);
    GXSetChanCtrl(GX_COLOR1A1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
                  GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_SPOT);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{40, 50, 60, 200});
    GXSetChanMatColor(GX_COLOR0A0, GXColor{200, 180, 160, 255});
    GXSetChanAmbColor(GX_COLOR1A1, GXColor{40, 50, 60, 200});
    GXSetChanMatColor(GX_COLOR1A1, GXColor{200, 180, 160, 255});
    GXLightObj light;
    GXInitLightPos(&light, 0.0f, 200.0f, 400.0f);
    GXInitLightAttn(&light, 2.0f, -1.0f, 0.5f, 1.0f, 0.5f, 0.0f);
    GXInitLightColor(&light, GXColor{255, 240, 220, 255});
    GXLoadLightObjImm(&light, GX_LIGHT0);
    GXInitLightPos(&light, -300.0f, -100.0f, 200.0f);
    GXInitLightColor(&light, GXColor{120, 140, 255, 255});
    GXLoadLightObjImm(&light, GX_LIGHT1);

    GXSetNumTexGens(2);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 30, GX_FALSE, 0);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, 33, GX_FALSE, 0);

    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetCullMode(GX_CULL_BACK);

    // Warm-up frame (pipelines/buffers sized) + timed run. Per-phase timers
    // split the vertex pipeline (GXBegin..GXEnd: capture + buildVertex) from
    // the per-draw renderer flush (endPass) and the rest of the frame
    // (beginFrame/endFrame — on lavapipe that includes software-raster waits,
    // so the split is what matters, not the wall total).
    double gxLoopMs = 0.0, endPassMs = 0.0, beginMs = 0.0, endMs = 0.0;
    const auto oneFrame = [&](bool timed) {
        const auto b0 = std::chrono::steady_clock::now();
        r.beginFrame();
        r.beginPass();
        const auto b1 = std::chrono::steady_clock::now();
        for (int s = 0; s < kStrips; ++s) {
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, kVertsPerStrip);
            for (int v = 0; v < kVertsPerStrip; ++v) {
                GXPosition1x16(static_cast<unsigned short>(v));
                GXNormal1x16(static_cast<unsigned short>(v));
                GXColor1x16(static_cast<unsigned short>(v));
                GXTexCoord1x16(static_cast<unsigned short>(v));
            }
            GXEnd();
        }
        const auto b2 = std::chrono::steady_clock::now();
        r.endPass();
        const auto b3 = std::chrono::steady_clock::now();
        r.endFrame();
        GXCompatEndFrame();
        const auto b4 = std::chrono::steady_clock::now();
        if (timed) {
            using d = std::chrono::duration<double, std::milli>;
            beginMs += d(b1 - b0).count();
            gxLoopMs += d(b2 - b1).count();
            endPassMs += d(b3 - b2).count();
            endMs += d(b4 - b3).count();
        }
    };
    oneFrame(false);
    oneFrame(false);

    constexpr int kDefaultFrames = 60;
    int kFrames = kDefaultFrames;
    if (const char* envFrames = std::getenv("LUMA_BENCH_FRAMES")) {
        const int parsed = std::atoi(envFrames);
        if (parsed > 0) {
            kFrames = parsed;
        }
    }
    for (int f = 0; f < kFrames; ++f) {
        oneFrame(true);
    }
    const double n = static_cast<double>(kFrames);
    std::printf("BENCH planets-like: %d strips x %d verts = %d verts/frame (build: %s)\n"
                "  GX capture+buildVertex : %7.3f ms/frame\n"
                "  endPass (per-draw)     : %7.3f ms/frame\n"
                "  beginFrame             : %7.3f ms/frame\n"
                "  endFrame (+GPU wait)   : %7.3f ms/frame\n"
                "  TOTAL                  : %7.3f ms/frame\n",
                kStrips, kVertsPerStrip, kStrips * kVertsPerStrip,
#if defined(NDEBUG)
                "optimized (NDEBUG)",
#else
                "DEBUG (unoptimized)",
#endif
                gxLoopMs / n, endPassMs / n, beginMs / n, endMs / n,
                (gxLoopMs + endPassMs + beginMs + endMs) / n);
    PL_LOG_INFO("bench", "planets-like: %d verts/frame -> gx %.3f ms, endPass %.3f ms (build: %s)",
                kStrips * kVertsPerStrip, gxLoopMs / n, endPassMs / n,
#if defined(NDEBUG)
                "optimized"
#else
                "DEBUG"
#endif
    );

    GXCompatShutdown();  // release sDynVb/white-fallback BEFORE the device goes
    Platform::Renderer::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
}
