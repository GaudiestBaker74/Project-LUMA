// =============================================================================
// galaxy-pc — PC entry point (Milestone 5.1: GX immediate vertices).
//
// M5.1 scope: the M4 demo (SDL3 window, Vulkan swapchain, fixed 60 Hz loop,
// Platform::Renderer) now drives its geometry through the real GX path:
// GXInit -> GXSetVtxDesc/GXSetVtxAttrFmt -> GXBegin + immediate writers
// (GXPosition3f32 / GXColor4u8) -> GXEnd. A rotating quad replaces the old
// hardcoded triangle; the clear color and viewport still travel through
// compat/gx (GXClearColor / GXSetViewport / GXClear) like the game boot.
//
// Usage:
//   galaxy-pc [--help] [--version] [--log-level LVL] [--log-file PATH]
//             [--assets-dir DIR] [--gpu-debug] [--width N] [--height N]
//             [--no-vsync] [--fullscreen] [--frames N] [--boot]
// =============================================================================

#include "compat/dvd/DVDCompat.h"
#include "compat/gx/GXCompat.h"
#include "compat/kpad/KPADCompat.h"
#include "compat/os/OSCompat.h"
#include "platform/Input/Input.h"
#include "platform/Renderer/Renderer.h"
#include "platform/Window/Window.h"
#include "platform/platform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// M9.2+: the vendored GameSystem.cpp `main()` (the DOL entry point), renamed
// by the PC_PORT patch. Runs the console boot prologue and then the frame
// loop forever (it does not return; the process is terminated by the OS/user).
void gameMain(void);

namespace {

constexpr const char* kVersion = "0.5.0 (Milestone 5)";

struct Options {
    std::string logLevel;
    std::string logFile;
    std::string assetsDir;
    int width = 1280;
    int height = 720;
    bool gpuDebug = false;
    bool vsync = true;
    bool fullscreen = false;
    int maxFrames = 0; // 0 = run until quit
    bool boot = false; // M9: run the real game boot (gameMain) instead of the demo
};

void printHelp() {
    std::printf(
        "galaxy-pc %s\n"
        "Native PC port of Super Mario Galaxy 1 (based on Petari).\n\n"
        "Usage: galaxy-pc [options]\n"
        "  --help             show this help\n"
        "  --version          print version\n"
        "  --log-level LVL    TRACE|DEBUG|INFO|WARN|ERROR|FATAL (default INFO)\n"
        "  --log-file PATH    also write logs to PATH\n"
        "  --assets-dir DIR   game assets root (default ./assets, env GALAXY_ASSETS_DIR)\n"
        "  --gpu-debug        enable Vulkan validation layers + debug labels\n"
        "  --width N          window width (default 1280)\n"
        "  --height N         window height (default 720)\n"
        "  --no-vsync         disable vsync (VK_PRESENT_MODE_IMMEDIATE)\n"
        "  --fullscreen       start fullscreen (F11 toggles)\n"
        "  --frames N         run N frames then exit cleanly (0 = run forever)\n"
        "  --boot             run the real game boot (M9: gameMain -> frameLoop,\n"
        "                     Logo scene) instead of the M5 demo\n\n"
        "M5 demo: SDL3 window + Vulkan (Platform::Renderer) + fixed 60 Hz loop\n"
        "with a rotating GX quad (immediate vertices). Esc/close quits; F11 toggles\n"
        "fullscreen.\n"
        "M7: the DVD layer mounts the assets tree (docs/assets.md); check your\n"
        "extraction with build/src/tools/verify-assets.\n",
        kVersion);
}

bool parseArgs(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            printHelp();
            return false; // caller exits 0
        }
        if (arg == "--version") {
            std::printf("galaxy-pc %s\n", kVersion);
            return false;
        }
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--log-level") {
            const char* v = next("--log-level");
            if (!v) return false;
            out.logLevel = v;
        } else if (arg == "--log-file") {
            const char* v = next("--log-file");
            if (!v) return false;
            out.logFile = v;
        } else if (arg == "--assets-dir") {
            const char* v = next("--assets-dir");
            if (!v) return false;
            out.assetsDir = v;
        } else if (arg == "--width") {
            const char* v = next("--width");
            if (!v) return false;
            out.width = std::atoi(v);
        } else if (arg == "--height") {
            const char* v = next("--height");
            if (!v) return false;
            out.height = std::atoi(v);
        } else if (arg == "--gpu-debug") {
            out.gpuDebug = true;
        } else if (arg == "--no-vsync") {
            out.vsync = false;
        } else if (arg == "--fullscreen") {
            out.fullscreen = true;
        } else if (arg == "--boot") {
            out.boot = true;
        } else if (arg == "--frames") {
            const char* v = next("--frames");
            if (!v) return false;
            out.maxFrames = std::atoi(v);
            if (out.maxFrames < 0) {
                std::fprintf(stderr, "--frames must be >= 0\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "unknown argument '%s' (see --help)\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// M5.3 demo geometry: the quad lives in GX vertex arrays (GXSetArray) and is
// referenced with 16-bit indices (GX_INDEX16); its texcoords are emitted
// directly (GX_TEXCOORD0) and textured with a procedural RGB565 GX texture.
// Rotation is applied on the CPU to the position array each frame, exactly
// like a display object would transform its vertices.
const float kQuadBase[4][2] = {
    {-0.6f, -0.6f},
    { 0.6f, -0.6f},
    { 0.6f,  0.6f},
    {-0.6f,  0.6f},
};

alignas(16) float sQuadPos[4][3]; // filled each frame with the rotated corners
// White vertex color: the textured shader outputs the texel color (modulate
// with 1.0), so the procedural texture shows pure.
const u8 sQuadColor[4][4] = {
    {255, 255, 255, 255},
    {255, 255, 255, 255},
    {255, 255, 255, 255},
    {255, 255, 255, 255},
};

// --- procedural GX texture (RGB565, 16x16, GX tiled 4x4 blocks) --------------
// Pattern: white 1px border + 4x4 checker of red/blue cells.
constexpr u32 kTexSize = 16;

// Writes a 16-bit BE RGB565 value for texel (tx,ty) into the tiled buffer.
void texPut565(u8* tiled, u32 tx, u32 ty, u16 rgb565) {
    const size_t block = (static_cast<size_t>(ty / 4) * (kTexSize / 4) + (tx / 4)) * 32;
    u8* p = tiled + block + ((ty % 4) * 4 + (tx % 4)) * 2;
    p[0] = static_cast<u8>(rgb565 >> 8);
    p[1] = static_cast<u8>(rgb565 & 0xFF);
}

u8 sTexTiled[kTexSize * kTexSize * 2]; // 32 bytes per 4x4 block
GXTexObj sDemoTexObj;

void initDemoTexture() {
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const bool border = (x == 0 || y == 0 || x == kTexSize - 1 || y == kTexSize - 1);
            const bool redCell = ((x / 4 + y / 4) % 2) == 0;
            const u16 c = border ? 0xFFFFu : (redCell ? 0xF800u : 0x001Fu);
            texPut565(sTexTiled, x, y, c);
        }
    }
    GXInitTexObj(&sDemoTexObj, sTexTiled, kTexSize, kTexSize, GX_TF_RGB565,
                 GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&sDemoTexObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE,
                    GX_FALSE, GX_ANISO_1);
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        return 0; // --help/--version or error already printed
    }

    // --- platform + compat OS boot ------------------------------------------
    Platform::Log::Config logConfig;
    if (!opts.logLevel.empty()) {
        const Platform::Log::Level level = Platform::Log::levelFromString(opts.logLevel.c_str());
        if (level != Platform::Log::Level::Count) {
            logConfig.minLevel = level;
        } else {
            std::fprintf(stderr, "unknown log level '%s' (TRACE|DEBUG|INFO|WARN|ERROR|FATAL)\n",
                         opts.logLevel.c_str());
            return 1;
        }
    }
    if (!opts.logFile.empty()) {
        logConfig.filePath = opts.logFile;
    }
    Platform::init(logConfig); // also initializes logging (re-applies config)
    if (!opts.assetsDir.empty()) {
        // Override the default "./assets" (or GALAXY_ASSETS_DIR) root.
        Platform::Filesystem::setRootDir(opts.assetsDir);
    }
    compat::initOS();
    compat::initDVD();  // M7: FST from the mounted assets root (no-op without assets)

    // --- M9: the real game boot (gameMain) -----------------------------------
    // The vendored boot prologue (DVDInit/VIInit/heaps/GameSystem::init) and
    // its frame loop need the window + renderer: the GX compat layer routes
    // the game's draws into the EFB pass and GXCopyDisp presents through the
    // swapchain (the frame cycle is bridged in the patched MainLoopFramework).
    // gameMain() never returns — like the console, the boot runs until the
    // process is terminated (window-close handling arrives with the M9.5
    // present-driven mode).
    if (opts.boot) {
        Platform::WindowConfig bootWindowConfig;
        bootWindowConfig.title = "galaxy-pc — Super Mario Galaxy (boot)";
        bootWindowConfig.width = opts.width;
        bootWindowConfig.height = opts.height;
        bootWindowConfig.startFullscreen = opts.fullscreen;
        Platform::Window bootWindow(bootWindowConfig);
        if (!bootWindow.handle()) {
            return 1;
        }

        Platform::RendererConfig bootRendererConfig;
        bootRendererConfig.enableValidation = opts.gpuDebug;
        bootRendererConfig.vsync = opts.vsync;
        if (!Platform::Renderer::init(bootWindow.handle(), bootRendererConfig)) {
            PL_LOG_FATAL("main", "renderer initialization failed");
            return 1;
        }

        PL_LOG_INFO("main", "--boot: entering gameMain() (the vendored game boot)");
        gameMain(); // does not return
        return 0;   // unreachable
    }

    // --- window + renderer ---------------------------------------------------
    Platform::WindowConfig windowConfig;
    windowConfig.title = "galaxy-pc — M5 (SDL3 + Vulkan)";
    windowConfig.width = opts.width;
    windowConfig.height = opts.height;
    windowConfig.startFullscreen = opts.fullscreen;
    Platform::Window window(windowConfig);
    if (!window.handle()) {
        return 1;
    }

    Platform::RendererConfig rendererConfig;
    rendererConfig.enableValidation = opts.gpuDebug;
    rendererConfig.vsync = opts.vsync;
    if (!Platform::Renderer::init(window.handle(), rendererConfig)) {
        PL_LOG_FATAL("main", "renderer initialization failed");
        return 1;
    }
    Platform::Renderer& renderer = Platform::Renderer::instance();

    // --- M5.3: configure the GX state machine (done once) -------------------
    // Vertex descriptor: position (INDEX16) + color0 (INDEX8) in arrays +
    // texcoord0 direct. The quad is textured with TEXMAP0.
    GXInit(nullptr, 0);
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    // Attribute formats on VTXFMT0: pos XYZ f32, color0 RGBA (u8x4), tex0 ST.
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetArray(GX_VA_POS, sQuadPos, sizeof(sQuadPos[0]));
    GXSetArray(GX_VA_CLR0, sQuadColor, sizeof(sQuadColor[0]));
    initDemoTexture();
    GXLoadTexObj(&sDemoTexObj, GX_TEXMAP0);
    // Texgen: identity matrix — UVs pass through from the vertex stream.
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE,
                      GX_PTIDENTITY);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    // --- M5.4: TEV chain ----------------------------------------------------
    // Stage 0: MODULATE — TEXMAP0 texel × vertex color (RASC).
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    // Stage 1: prev = lerp(prev, ONE, KONST) with K2 = (255,128,64): the
    // checkerboard result is pulled toward the K constant — a chained stage
    // exercising the K constant + register routing.
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_ONE, GX_CC_KONST, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_APREV, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K2);
    GXSetTevKColor(GX_KCOLOR2, GXColor{255, 128, 64, 255});
    GXSetNumTevStages(2);

    // --- fixed-timestep game loop (60 Hz) ------------------------------------
    constexpr double kStepSeconds = 1.0 / 60.0;
    double accumulator = 0.0;
    double angle = 0.0;
    Platform::Timing::TimePoint lastTime = Platform::Timing::now();

    int lastW = window.width();
    int lastH = window.height();

    int fpsFrames = 0;
    double fpsTimer = 0.0;

    Platform::Input input;
    // M6: wire the compat input layer (KPAD/WPAD) to the raw device layer.
    // Rumble requests from the game reach the SDL gamepad through `input`.
    // (A capture-less lambda stored as a function pointer cannot capture
    // `input`, so it goes through a file-static pointer set below.)
    static Platform::Input* sRumbleInput = nullptr;
    sRumbleInput = &input;
    Platform::CompatInput::init();
    Platform::CompatInput::setRumbleSink([](int gamepadIndex, bool on) {
        if (sRumbleInput != nullptr) {
            sRumbleInput->setRumble(gamepadIndex, on);
        }
    });
    bool running = true;
    int framesRendered = 0;

    PL_LOG_INFO("main", "galaxy-pc %s — demo running (Esc/close to quit, F11 fullscreen)", kVersion);

    while (running) {
        // --- input ----------------------------------------------------------
        const Platform::InputState inputState = input.poll(window);
        if (inputState.quit) {
            running = false;
        }
        if (inputState.fullscreenToggle) {
            window.toggleFullscreen();
        }

        // --- fixed timestep -------------------------------------------------
        const Platform::Timing::TimePoint now = Platform::Timing::now();
        double dt = Platform::Timing::secondsBetween(lastTime, now);
        lastTime = now;
        if (dt > 0.25) {
            dt = 0.25; // clamp after pauses/hitches (avoid spiral of death)
        }
        Platform::CompatInput::updateFrame(inputState, dt);
        accumulator += dt;

        while (accumulator >= kStepSeconds) {
            // Fixed 60 Hz step: advance the demo rotation.
            angle += 0.03; // ~1.8 deg per step -> ~1 revolution every 20 s
            accumulator -= kStepSeconds;
        }

        // --- resize handling ------------------------------------------------
        const int w = window.width();
        const int h = window.height();
        if (w != lastW || h != lastH) {
            lastW = w;
            lastH = h;
            renderer.onResize(w, h);
        }

        // --- render through the GX smell ------------------------------------
        if (!renderer.beginFrame()) {
            continue; // swapchain was recreated or is out of date
        }

        // M5.7c: the scene renders into the EFB (offscreen render target, GX
        // space 640x448) and GXCopyDisp blits it to the swapchain, like the
        // console: draw -> copy -> present (docs/gx.md §J).
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(Platform::Timing::nowSeconds()));
        GXClearColor(GXColor{static_cast<u8>(0x30 + 0x60 * pulse),
                             static_cast<u8>(0x30 + 0x40 * (1.0f - pulse)),
                             0x40, 0xFF});
        GXSetViewport(0.0f, 0.0f, 640.0f, 448.0f, 0.0f, 1.0f); // EFB space
        GXClear(GX_CLEAR_COLOR);

        renderer.beginPass(static_cast<Platform::RenderTargetHandle>(
            Platform::CompatGx::getEfbRenderTarget()));

        // Rotate the quad corners into the position array (CPU transform,
        // like the game's display objects), then emit the quad by indices.
        const float c = static_cast<float>(std::cos(angle));
        const float s = static_cast<float>(std::sin(angle));
        for (int i = 0; i < 4; ++i) {
            sQuadPos[i][0] = kQuadBase[i][0] * c - kQuadBase[i][1] * s;
            sQuadPos[i][1] = kQuadBase[i][0] * s + kQuadBase[i][1] * c;
            sQuadPos[i][2] = 0.0f;
        }
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        for (int i = 0; i < 4; ++i) {
            GXPosition1x16(static_cast<u16>(i));
            GXColor1x8(static_cast<u8>(i));
            GXTexCoord2f32(uv[i][0], uv[i][1]);
        }
        GXEnd();

        renderer.endPass();
        GXCopyDisp(nullptr, GX_TRUE); // blit EFB -> swapchain (present)
        renderer.endFrame();
        GXCompatEndFrame(); // reset the dynamic vertex buffer cursor

        ++framesRendered;
        if (opts.maxFrames > 0 && framesRendered >= opts.maxFrames) {
            PL_LOG_INFO("main", "requested %d frames rendered — exiting", framesRendered);
            running = false;
        }

        // --- FPS overlay via log (every second) ------------------------------
        ++fpsFrames;
        fpsTimer += dt;
        if (fpsTimer >= 1.0) {
            const Platform::FrameStats& fs = renderer.lastFrameStats();
            PL_LOG_INFO("main",
                        "fps: %.1f (w=%d h=%d) | cpu-render %.2f ms | gpu %.2f ms | vram %llu/%llu MB",
                        fpsFrames / fpsTimer, lastW, lastH, fs.cpuRenderMs, fs.gpuMs,
                        static_cast<unsigned long long>(fs.memoryUsed / (1024 * 1024)),
                        static_cast<unsigned long long>(fs.memoryBudget / (1024 * 1024)));
            fpsFrames = 0;
            fpsTimer = 0.0;
        }
    }

    // --- clean shutdown ------------------------------------------------------
    PL_LOG_INFO("main", "shutting down");
    GXCompatShutdown(); // release the GX dynamic vertex buffer
    compat::shutdownDVD();  // stop the DVD async worker (drains/cancels reads)
    Platform::CompatInput::shutdown();
    input.shutdown();
    Platform::Renderer::shutdown();
    Platform::shutdown();
    return 0;
}
