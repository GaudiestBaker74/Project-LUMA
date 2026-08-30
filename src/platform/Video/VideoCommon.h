#pragma once
// =============================================================================
// Platform::Video — internal Vulkan helpers shared by the video backend and
// the offscreen smoke test. Not part of the public platform API.
// =============================================================================

#include "platform/Log/Log.h"

#include <cstdio>
#include <vector>

// volk provides Vulkan function pointers; VK_NO_PROTOTYPES is handled by
// volk.h itself (it loads everything at runtime, no linker dependency).
#include "volk.h"

namespace Platform::VideoDetail {

// Logs the failed expression and aborts (FATAL). Only for programming errors
// in setup code; runtime errors (e.g. swapchain out-of-date) are handled
// explicitly by the caller.
// (stringify is defined below vkCheck; this forward declaration lets vkCheck
// use it without an include-order dance)
inline const char* stringify(VkResult result);

inline void vkCheck(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        PL_LOG_FATAL("video", "Vulkan error %d (%s) at %s:%d in: %s", static_cast<int>(result),
                     stringify(result), file, line, expr);
    }
}

#define VK_CHECK(expr) ::Platform::VideoDetail::vkCheck((expr), #expr, __FILE__, __LINE__)

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
        PL_LOG_FATAL("video", "no Vulkan physical devices found (is a driver/ICD installed?)");
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

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 100;
        }
        if (score > bestScore) {
            bestScore = score;
            best = dev;
            bestGfx = static_cast<uint32_t>(gfx);
            bestPresent = static_cast<uint32_t>(present);
        }
    }
    if (best == VK_NULL_HANDLE) {
        PL_LOG_FATAL("video", "no suitable Vulkan device with graphics+present queues");
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    PL_LOG_INFO("video", "GPU: %s (type %d)", props.deviceName, static_cast<int>(props.deviceType));

    if (outGraphicsFamily) {
        *outGraphicsFamily = bestGfx;
    }
    if (outPresentFamily) {
        *outPresentFamily = bestPresent;
    }
    return best;
}

} // namespace Platform::Video::Detail
