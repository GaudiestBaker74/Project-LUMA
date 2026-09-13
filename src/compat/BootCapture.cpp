// =============================================================================
// compat/BootCapture — see BootCapture.h for the design.
// =============================================================================

#include "compat/BootCapture.h"

#include "compat/HostShutdown.h"
#include "compat/gx/GXCompat.h"
#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace compat {
namespace {

std::string sPath;      // explicit output path ("" = none; auto-name per dump)
u32 sTargetFrame = 0;    // timed capture: present index to capture
bool sExitAfter = false; // timed capture: exit once captured
bool sArmed = false;     // timed capture armed (setBootCapture)
bool sDone = false;      // timed dump already written
bool sExitFired = false; // shutdown already requested
bool sPendingDump = false; // hotkey (F12) dump requested for the next present
u32 sWaitedNoEfb = 0;      // presents skipped because they had no EFB pass

} // namespace

bool writeEfbPpm(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        PL_LOG_WARN("capture", "EFB dump: no output path (nothing written)");
        return false;
    }
    Platform::Renderer& renderer = Platform::Renderer::instance();
    if (!renderer.isInitialized()) {
        PL_LOG_WARN("capture", "EFB dump: renderer not initialized");
        return false;
    }
    auto efb = static_cast<Platform::RenderTargetHandle>(Platform::CompatGx::getEfbRenderTarget());
    if (efb == nullptr) {
        PL_LOG_WARN("capture", "EFB dump: no EFB render target (nothing written)");
        return false;
    }
    const int width = Platform::CompatGx::getEfbWidth();
    const int height = Platform::CompatGx::getEfbHeight();
    if (width <= 0 || height <= 0) {
        PL_LOG_WARN("capture", "EFB dump: EFB size unknown (%dx%d)", width, height);
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    if (!renderer.readRenderTarget(efb, 0, 0, static_cast<u32>(width), static_cast<u32>(height),
                                   pixels.data())) {
        PL_LOG_WARN("capture", "EFB dump: readback failed (%dx%d)", width, height);
        return false;
    }

    FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        PL_LOG_WARN("capture", "EFB dump: cannot write '%s'", path);
        return false;
    }
    std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        std::fwrite(&pixels[i], 1, 3, fp); // drop alpha
    }
    std::fclose(fp);
    PL_LOG_INFO("capture", "EFB dump: wrote '%s' (%dx%d)", path, width, height);
    return true;
}

void requestFrameDump() {
    if (sExitFired) {
        return;
    }
    if (sPendingDump) {
        return; // already queued for the next present
    }
    sPendingDump = true;
    PL_LOG_INFO("capture", "frame dump requested — writing the next presented EFB frame%s",
                sPath.empty() ? " (auto name luma-frame-<present>.ppm)" : "");
}

void setBootCapture(const char* pPath, u32 frame, bool exitAfter) {
    sPath = (pPath != nullptr) ? pPath : "";
    sTargetFrame = frame;
    sExitAfter = exitAfter;
    sArmed = !sPath.empty() || sExitAfter;
    sDone = false;
    sExitFired = false;
    if (!sArmed) {
        return;
    }
    PL_LOG_INFO("capture", "boot capture armed: dump='%s' at present #%u, exit-after=%s",
                sPath.empty() ? "(none)" : sPath.c_str(), sTargetFrame,
                sExitAfter ? "yes" : "no");
}

bool hasBootCapture() { return sArmed; }

void notifyBootPresent(u32 presentIndex, bool efbPresented) {
    if (sExitFired) {
        return;
    }
    const bool timed = sArmed && presentIndex >= sTargetFrame;
    if (!timed && !sPendingDump) {
        return;
    }
    const bool wantDump = sPendingDump || (!sPath.empty() && !sDone);

    // Exit-only request (--frames N with no --screenshot): nothing to write and
    // no reason to wait for a frame that has EFB content.
    if (!wantDump) {
        sExitFired = true;
        PL_LOG_INFO("capture", "boot capture: reached present #%u — shutting down", sTargetFrame);
        shutdownHostForExit();
        return;
    }

    if (!efbPresented) {
        // Wait for a frame that actually has EFB content (the first presents can
        // be swapchain-only while the scene sets itself up).
        if (++sWaitedNoEfb == 60) {
            PL_LOG_WARN("capture", "no EFB pass in the last 60 presents — frame dump still waiting "
                                   "(present #%u)", presentIndex);
            sWaitedNoEfb = 0;
        }
        return;
    }
    sWaitedNoEfb = 0;

    const bool hotkey = sPendingDump;
    sPendingDump = false;

    // One file per dump: the explicit path the first time, auto-named after
    // that (or always auto-named when no path was given).
    std::string path;
    if (!sPath.empty() && !sDone) {
        path = sPath;
        sDone = true;
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "luma-frame-%05u.ppm", presentIndex);
        path = buf;
    }

    Platform::Renderer::instance().flushFrame(); // make the readback see THIS frame
    if (writeEfbPpm(path.c_str()) && hotkey) {
        PL_LOG_INFO("capture", "hotkey frame dump at present #%u -> '%s'", presentIndex,
                    path.c_str());
    }

    if (timed && sExitAfter && !sPath.empty()) {
        sExitFired = true;
        PL_LOG_INFO("capture", "boot capture: reached present #%u — shutting down", sTargetFrame);
        shutdownHostForExit();
    }
}

} // namespace compat
