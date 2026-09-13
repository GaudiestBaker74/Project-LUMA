#pragma once

#include <cstdint>

namespace Platform::CompatScreen {

// One presentation policy shared by the renderer-facing GX bridge, the
// vendored ScreenUtil entry points, 2D layouts and the title camera. The
// framebuffer fields describe the Wii-compatible EFB; output fields describe
// the live host swapchain. The EFB is presented into a centered, aspect-
// preserving rectangle, so the logical layout space stays Wii-compatible
// instead of changing width when a desktop window is resized.
struct Metrics {
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t framebufferWidth = 640;
    std::uint32_t framebufferHeight = 456;
    std::uint32_t logicalWidth = 608;
    std::uint32_t logicalHeight = 456;
    float framebufferAspect = 640.0f / 456.0f;
    float outputAspect = 608.0f / 456.0f;
};

// Returns a snapshot. It is intentionally computed on demand: Renderer::onResize
// recreates the swapchain and updates its extent without rebuilding any game
// object or layout, so the next frame automatically observes the new policy.
Metrics getMetrics();

// Pure helpers kept public for headless tests and for callers that need the
// same Wii-coordinate/16:9 decision without touching SDL or Vulkan state.
std::uint32_t logicalWidthForAspect(float aspect);
bool isWidescreenAspect(float aspect);

} // namespace Platform::CompatScreen
