// =============================================================================
// galaxy-pc — PC entry point (Milestone 4.1: Platform::Renderer API).
//
// M4.1 scope: the M3 demo (SDL3 window, Vulkan swapchain, fixed 60 Hz loop)
// now runs through the closed Platform::Renderer API: a cached pipeline, a
// vertex buffer, per-frame rotation pushed as a uniform (push constant) and
// draw()/endPass()/endFrame(). The clear color and viewport still travel
// through the compat/gx "first GX smell" (GXClearColor / GXSetViewport /
// GXClear) exactly like the game boot will call them.
//
// Usage:
//   galaxy-pc [--help] [--version] [--log-level LVL] [--log-file PATH]
//             [--assets-dir DIR] [--gpu-debug] [--width N] [--height N]
//             [--no-vsync] [--fullscreen] [--frames N]
// =============================================================================

#include "compat/gx/GXCompat.h"
#include "compat/os/OSCompat.h"
#include "platform/Input/Input.h"
#include "platform/Renderer/Renderer.h"
#include "platform/Renderer/vk_demo_shaders.h"
#include "platform/Window/Window.h"
#include "platform/platform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char* kVersion = "0.4.0 (Milestone 4)";

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
        "  --frames N         run N frames then exit cleanly (0 = run forever)\n\n"
        "M4 demo: SDL3 window + Vulkan (Platform::Renderer) + fixed 60 Hz loop\n"
        "with a rotating triangle. Esc/close quits; F11 toggles fullscreen.\n",
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

// Demo triangle geometry (same as M3): pos(2f) + color(3f) = 20 bytes.
struct DemoVertex {
    float x, y;
    float r, g, b;
};

const DemoVertex kTriangleVertices[] = {
    {  0.0f, -0.6f,   1.0f, 0.2f, 0.2f },
    {  0.6f,  0.6f,   0.2f, 1.0f, 0.2f },
    { -0.6f,  0.6f,   0.2f, 0.2f, 1.0f },
};

// Column-major mat3 (std430: 3 columns x 16 bytes). Rotation by `angle`.
void buildRotationMatrix(float angle, float out[12]) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    out[0] = c;  out[1] = s;  out[2] = 0.0f; out[3] = 0.0f;   // column 0
    out[4] = -s; out[5] = c;  out[6] = 0.0f; out[7] = 0.0f;   // column 1
    out[8] = 0.0f; out[9] = 0.0f; out[10] = 1.0f; out[11] = 0.0f; // column 2
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

    // --- window + renderer ---------------------------------------------------
    Platform::WindowConfig windowConfig;
    windowConfig.title = "galaxy-pc — M4 (SDL3 + Vulkan)";
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

    // Demo pipeline (cached; requested once — later frames reuse the same
    // PipelineHandle via getOrCreatePipeline).
    Platform::PipelineDesc pipelineDesc;
    pipelineDesc.vertexLayout.stride = sizeof(DemoVertex);
    pipelineDesc.vertexLayout.attribs = {
        {0, 0, Platform::VertexFormat::R32G32_SFLOAT},
        {1, 8, Platform::VertexFormat::R32G32B32_SFLOAT},
    };
    pipelineDesc.vertSpv = kTriangleVertSpv;
    pipelineDesc.vertSpvSize = sizeof(kTriangleVertSpv);
    pipelineDesc.fragSpv = kTriangleFragSpv;
    pipelineDesc.fragSpvSize = sizeof(kTriangleFragSpv);
    pipelineDesc.colorFormat = renderer.passColorFormat();
    Platform::PipelineHandle pipeline = renderer.getOrCreatePipeline(pipelineDesc);
    if (!pipeline) {
        PL_LOG_FATAL("main", "failed to create demo pipeline");
        return 1;
    }

    Platform::BufferHandle vertexBuffer =
        renderer.createBuffer(Platform::BufferUsage::Vertex, sizeof(kTriangleVertices),
                              kTriangleVertices);
    if (!vertexBuffer) {
        PL_LOG_FATAL("main", "failed to create vertex buffer");
        return 1;
    }

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

        // Pulsing clear color (wall time; the triangle rotation follows the
        // fixed step).
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(Platform::Timing::nowSeconds()));
        GXClearColor(GXColor{static_cast<u8>(0x30 + 0x60 * pulse),
                             static_cast<u8>(0x30 + 0x40 * (1.0f - pulse)),
                             0x40, 0xFF});
        GXSetViewport(0.0f, 0.0f, static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f);
        GXClear(GX_CLEAR_COLOR);

        renderer.beginPass(); // clears with the GXClearColor stored above
        renderer.bindPipeline(pipeline);
        renderer.bindVertexBuffer(vertexBuffer, 0);
        float rot[12];
        buildRotationMatrix(static_cast<float>(angle), rot);
        renderer.setUniforms(rot, sizeof(rot));
        renderer.draw(3, 0);
        renderer.endPass();
        renderer.endFrame();

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
    renderer.destroyBuffer(vertexBuffer);
    Platform::Renderer::shutdown();
    Platform::shutdown();
    return 0;
}
