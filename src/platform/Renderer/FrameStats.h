#pragma once
// =============================================================================
// Platform::FrameStats — frame timing + memory budget statistics (M4.3).
//
// Pure data + pure helpers (no Vulkan types) so the conversion math is
// unit-testable headless. The renderer fills the struct every frame; main.cpp
// logs it in the FPS overlay.
// =============================================================================

#include <cstdint>

namespace Platform {

struct FrameStats {
    double cpuRenderMs = 0.0;  // CPU time in the render path (acquire..submit, excl. present wait)
    double gpuMs = 0.0;        // GPU time for the frame (0 if timestamps unsupported)
    uint64_t memoryUsed = 0;   // device-local memory in use (0 if budget unsupported)
    uint64_t memoryBudget = 0; // device-local memory budget (0 if budget unsupported)
};

// Converts a GPU timestamp delta (ticks) to milliseconds using the device's
// timestamp period (ns per tick). Pure — unit-testable without a device.
inline double gpuTicksToMs(uint64_t deltaTicks, float periodNs) {
    return static_cast<double>(deltaTicks) * static_cast<double>(periodNs) / 1e6;
}

} // namespace Platform
