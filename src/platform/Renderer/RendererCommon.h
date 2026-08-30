#pragma once
// =============================================================================
// Platform::RenderDetail — internal Vulkan helpers shared by the renderer
// backend and the offscreen smoke test. Not part of the public platform API.
//
// Named RenderDetail (not Renderer::Detail): `Renderer` is a class, so a
// nested `namespace Platform::Renderer::Detail` would be illegal (see
// docs/porting.md §3.1 quirk 13).
// =============================================================================

#include "platform/Log/Log.h"

#include <cstdio>
#include <vector>

// volk provides Vulkan function pointers; VK_NO_PROTOTYPES is handled by
// volk.h itself (it loads everything at runtime, no linker dependency).
#include "volk.h"

namespace Platform::RenderDetail {

// (stringify is defined below vkCheck; this forward declaration lets vkCheck
// use it without an include-order dance)
inline const char* stringify(VkResult result);

// Logs the failed expression and aborts (FATAL). Only for programming errors
// in setup code; runtime errors (e.g. swapchain out-of-date) are handled
// explicitly by the caller.
inline void vkCheck(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        PL_LOG_FATAL("renderer", "Vulkan error %d (%s) at %s:%d in: %s", static_cast<int>(result),
                     stringify(result), file, line, expr);
    }
}

#define VK_CHECK(expr) ::Platform::RenderDetail::vkCheck((expr), #expr, __FILE__, __LINE__)

inline const char* stringify(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "unknown";
    }
}

// Picks the best physical device: prefer a discrete GPU, must support the
// graphics queue family (present support is checked separately by the caller
// when a surface exists).
inline VkPhysicalDevice pickPhysicalDevice(VkInstance instance, VkSurfaceKHR surface,
                                           uint32_t* outGraphicsFamily,
                                           uint32_t* outPresentFamily) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        PL_LOG_FATAL("renderer", "no Vulkan physical devices found (is a driver/ICD installed?)");
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    int bestScore = -1;
    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t bestGfx = 0, bestPresent = 0;

    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, queues.data());

        int gfx = -1, present = -1;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gfx = static_cast<int>(i);
            }
            VkBool32 supportsPresent = VK_FALSE;
            if (surface) {
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &supportsPresent);
            } else {
                supportsPresent = VK_TRUE; // headless: any queue presents
            }
            if (supportsPresent == VK_TRUE && gfx == static_cast<int>(i)) {
                present = gfx;
            }
            if (gfx >= 0 && present >= 0) {
                break;
            }
        }
        if (gfx < 0 || present < 0) {
            continue;
        }

        // Score: discrete GPU > integrated; prefer lower family indices when
        // graphics+present differ (avoids concurrent sharing modes).
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 0;
        score += (gfx == present) ? 10 : 0;
        if (score > bestScore) {
            bestScore = score;
            best = dev;
            bestGfx = static_cast<uint32_t>(gfx);
            bestPresent = static_cast<uint32_t>(present);
        }
    }

    if (!best) {
        PL_LOG_FATAL("renderer", "no suitable Vulkan physical device");
    }
    // out-params are optional (e.g. headless tests pass nullptr for present).
    if (outGraphicsFamily) *outGraphicsFamily = bestGfx;
    if (outPresentFamily) *outPresentFamily = bestPresent;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    PL_LOG_INFO("renderer", "GPU: %s (type %d)", props.deviceName, static_cast<int>(props.deviceType));
    return best;
}

} // namespace Platform::RenderDetail
