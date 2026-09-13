// =============================================================================
// compat/game — one host presentation policy for ScreenUtil, GX/EFB and the
// title camera.
// =============================================================================

#include "compat/ScreenMetrics.h"

#include "platform/Renderer/Renderer.h"

#include <JSystem/JUtility/JUTVideo.hpp>

#include <cmath>

namespace Platform::CompatScreen {

namespace {

constexpr float kWiiLogicalWidth = 608.0f;
constexpr float kWiiLogicalHeight = 456.0f;
constexpr float kFallbackAspect = 608.0f / 456.0f;
constexpr std::uint32_t kFallbackFramebufferWidth = 640;
constexpr std::uint32_t kFallbackFramebufferHeight = 456;

float saneAspect(float aspect) {
    // A minimized/temporarily unavailable swapchain has no usable extent. Do
    // not let that transient state produce a zero projection or a huge integer
    // width; the renderer will publish the real extent on the next frame.
    return std::isfinite(aspect) && aspect > 0.01f ? aspect : kFallbackAspect;
}

} // namespace

std::uint32_t logicalWidthForAspect(float /*aspect*/) {
    // Layout coordinates are the Wii 4:3 virtual screen (608 x 456). A host
    // aspect change is handled by the presentation rectangle, not by changing
    // this coordinate range; changing it here would make every BRLYT pane
    // wider/narrower before the final blit and is exactly the logo-shear bug
    // this policy is meant to prevent.
    return static_cast<std::uint32_t>(kWiiLogicalWidth);
}

bool isWidescreenAspect(float aspect) {
    // Keep the game's 4:3/16:9 resource choice stable around the midpoint;
    // unlike the old hard-coded false, this follows a live resize and also
    // behaves sensibly for ultrawide and portrait windows.
    return saneAspect(aspect) >= 1.5555556f;
}

Metrics getMetrics() {
    Metrics out;
    out.outputAspect = kFallbackAspect;

    Renderer& renderer = Renderer::instance();
    if (renderer.isInitialized() && renderer.outputWidth() != 0 && renderer.outputHeight() != 0) {
        out.outputWidth = renderer.outputWidth();
        out.outputHeight = renderer.outputHeight();
        out.outputAspect = saneAspect(renderer.outputAspect());
    }

    // JUTVideo is the authoritative Wii-compatible EFB size for the boot
    // path. The native demo has no JUTVideo manager, so retain the same host
    // mode fallback used by GameSystem.cpp.
    JUTVideo* video = JUTVideo::getManager();
    if (video != nullptr && video->getRenderMode() != nullptr) {
        const GXRenderModeObj* mode = video->getRenderMode();
        if (mode->fbWidth != 0) {
            out.framebufferWidth = mode->fbWidth;
        }
        if (mode->efbHeight != 0) {
            out.framebufferHeight = mode->efbHeight;
        }
    } else {
        out.framebufferWidth = kFallbackFramebufferWidth;
        out.framebufferHeight = kFallbackFramebufferHeight;
    }

    out.logicalHeight = static_cast<std::uint32_t>(kWiiLogicalHeight);
    out.logicalWidth = logicalWidthForAspect(out.outputAspect);
    out.framebufferAspect = out.framebufferHeight != 0
                                ? static_cast<float>(out.framebufferWidth) /
                                      static_cast<float>(out.framebufferHeight)
                                : kFallbackAspect;
    return out;
}

} // namespace Platform::CompatScreen
