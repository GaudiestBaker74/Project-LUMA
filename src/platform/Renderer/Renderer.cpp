// =============================================================================
// Platform::Renderer — Vulkan backend (M4.2). See Renderer.h for the API.
//
// M4.1: device/swapchain/present, passes, buffers, pipeline cache by state
// hash, push-constant uniforms, draw/drawIndexed.
// M4.2 adds: textures (staging upload + debug labels), cached samplers,
// render targets (color + depth, the future EFB), and fragment texture
// sampling through one combined-image-sampler descriptor array per pipeline.
//
// Implementation notes:
//   * All Vulkan entry points come from volk (no link-time dependency).
//   * Frame sync: one frame in flight (fence), acquire->submit->present wired
//     with an image-available semaphore + per-image render-finished
//     semaphores (the M3 fix, preserved here).
//   * Handles are void* in the public header so it never depends on volk.
// =============================================================================

#include "platform/Renderer/Renderer.h"
#include "platform/Renderer/Format.h"
#include "platform/Renderer/RendererCommon.h"

#include "platform/Log/Log.h"
#include "platform/Timing/Timing.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Platform {

using namespace Platform::RenderDetail;

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";


// --- handle accessors -------------------------------------------------------
// Resolved through instance() so they work in static member functions too
// (init/shutdown), where `this` does not exist.
// (Fully qualified so the macros also work in free functions of the anonymous
// namespace, e.g. allocateMemory(), not only in member functions.)
#define VKINST   reinterpret_cast<VkInstance>(::Platform::Renderer::instance().mInstance)
#define VKDEV    reinterpret_cast<VkDevice>(::Platform::Renderer::instance().mDevice)
#define VKPHYS   reinterpret_cast<VkPhysicalDevice>(::Platform::Renderer::instance().mPhysicalDevice)
#define VKSURF   reinterpret_cast<VkSurfaceKHR>(::Platform::Renderer::instance().mSurface)
#define VKSWAP   reinterpret_cast<VkSwapchainKHR>(::Platform::Renderer::instance().mSwapchain)
#define VKGQ     reinterpret_cast<VkQueue>(::Platform::Renderer::instance().mGraphicsQueue)
#define VKPQ     reinterpret_cast<VkQueue>(::Platform::Renderer::instance().mPresentQueue)

// Push constants: one small block for the vertex stage (matrices/colors from
// compat/gx per draw). 128 bytes is the guaranteed minimum for a single stage.
constexpr uint32_t kPushConstantSize = 128;

// M5.4/M5.5 (TEV): per-draw fragment-UBO region and the per-frame arena size.
// The TEV UBO (compat/gx/GXTevInternal.h) is 1296 B; regions are rounded up
// to the dynamic-offset alignment.
constexpr uint64_t kTevUboRegionBytes = 2048;
constexpr uint64_t kTevUboArenaBytes = 1 << 20; // 1 MiB

// --- fixed-function state -> Vulkan mappings (M5.5) --------------------------
// The Platform enums are Vulkan-semantic; compat/gx maps the GX enums onto
// them (e.g. GX_BM_SUBTRACT, which computes dst − src, maps to
// BlendOp::ReverseSubtract).

VkCullModeFlags cullModeToVk(CullMode m) {
    switch (m) {
        case CullMode::None:         return VK_CULL_MODE_NONE;
        case CullMode::Back:         return VK_CULL_MODE_BACK_BIT;
        case CullMode::Front:        return VK_CULL_MODE_FRONT_BIT;
        case CullMode::FrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
    }
    return VK_CULL_MODE_NONE;
}

VkCompareOp compareOpToVk(CompareOp op) {
    switch (op) {
        case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:         return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkBlendOp blendOpToVk(BlendOp op) {
    switch (op) {
        case BlendOp::Add:             return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    }
    return VK_BLEND_OP_ADD;
}

// `dstAlphaConst`: GXSetDstAlpha enabled — the DSTALPHA/INVDSTALPHA factors
// then read the constant alpha (cb.blendConstants[3]) instead of the
// framebuffer alpha.
VkBlendFactor blendFactorToVk(BlendFactor f, bool dstAlphaConst) {
    switch (f) {
        case BlendFactor::Zero:                return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:                 return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:            return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha:            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:
            return dstAlphaConst ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha:
            return dstAlphaConst ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
                                 : VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantAlpha:         return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BlendFactor::OneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    // M5.2 dynamic buffers: persistent host mapping + allocation capacity
    // (capacity may exceed `size` after growth).
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
};

struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    Platform::TextureFormat format = Platform::TextureFormat::R8G8B8A8_UNORM;
};

struct GpuRenderTarget {
    GpuTexture color;
    GpuTexture depth; // only if desc.hasDepth
    bool hasDepth = false;
    // Current layout of the color image, tracked across passes/blits/readbacks
    // (validation requires barrier oldLayout to match the real layout; the
    // image is left in TRANSFER_SRC_OPTIMAL by blitPassToSwapchain and in
    // COLOR_ATTACHMENT_OPTIMAL by beginPass/readRenderTarget).
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    Platform::Log::Level level = Platform::Log::Level::Debug;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        level = Platform::Log::Level::Error;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        level = Platform::Log::Level::Warn;
    }
    PL_LOG(level, "renderer.vk", "%s", data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

// FNV-1a over the pipeline state — the cache key. (Equality is still checked
// on the stored desc to resolve collisions.)
uint64_t hashPipelineDesc(const PipelineDesc& d) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(&d.topology, sizeof(d.topology));
    mix(&d.vertexLayout.stride, sizeof(d.vertexLayout.stride));
    for (const auto& a : d.vertexLayout.attribs) {
        mix(&a, sizeof(a));
    }
    mix(&d.blendEnable, sizeof(d.blendEnable));
    mix(&d.srcBlendFactor, sizeof(d.srcBlendFactor));
    mix(&d.dstBlendFactor, sizeof(d.dstBlendFactor));
    mix(&d.blendOp, sizeof(d.blendOp));
    mix(&d.logicOpEnable, sizeof(d.logicOpEnable));
    mix(&d.logicOp, sizeof(d.logicOp));
    mix(&d.dstAlphaEnable, sizeof(d.dstAlphaEnable));
    mix(&d.dstAlphaValue, sizeof(d.dstAlphaValue));
    mix(&d.depthTest, sizeof(d.depthTest));
    mix(&d.depthWrite, sizeof(d.depthWrite));
    mix(&d.depthCompare, sizeof(d.depthCompare));
    mix(&d.cullMode, sizeof(d.cullMode));
    mix(&d.colorWrite, sizeof(d.colorWrite));
    mix(&d.alphaWrite, sizeof(d.alphaWrite));
    mix(&d.colorFormat, sizeof(d.colorFormat));
    mix(&d.depthFormat, sizeof(d.depthFormat));
    mix(&d.textureCount, sizeof(d.textureCount));
    mix(&d.fragmentUbo, sizeof(d.fragmentUbo));
    mix(&d.vertSpvSize, sizeof(d.vertSpvSize));
    mix(&d.fragSpvSize, sizeof(d.fragSpvSize));
    // A few words of each shader pin the version in the hash.
    if (d.vertSpv && d.vertSpvSize >= 8) mix(d.vertSpv, 8);
    if (d.fragSpv && d.fragSpvSize >= 8) mix(d.fragSpv, 8);
    return h;
}

uint64_t hashSamplerDesc(const SamplerDesc& d) {
    uint64_t h = 14695981039346656037ull;
    const auto* b = reinterpret_cast<const uint8_t*>(&d);
    for (size_t i = 0; i < sizeof(d); ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

// Friend of Renderer: backend helpers that need the private Vulkan state.
// (Declared as Platform::RendererAccess in the header; defined here in a
// named namespace so the friend declaration matches.)
struct RendererAccess {
    static VkDeviceMemory allocateMemory(VkDeviceSize size, uint32_t typeBits,
                                         VkMemoryPropertyFlags props) {
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = size;
        mai.memoryTypeIndex = findMemoryType(VKPHYS, typeBits, props);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            return VK_NULL_HANDLE;
        }
        VkDeviceMemory mem = VK_NULL_HANDLE;
        vkAllocateMemory(VKDEV, &mai, nullptr, &mem);
        return mem;
    }

    // M9.4: immediate destruction of a retired render target's GPU resources
    // (only called once the frame fence guarantees no submitted command
    // buffer references them — see Renderer::destroyRenderTarget).
    static void destroyRenderTargetNow(void* target) {
        auto* rt = reinterpret_cast<GpuRenderTarget*>(target);
        if (rt->hasDepth) {
            vkDestroyImageView(VKDEV, rt->depth.view, nullptr);
            vkDestroyImage(VKDEV, rt->depth.image, nullptr);
            vkFreeMemory(VKDEV, rt->depth.memory, nullptr);
        }
        vkDestroyImageView(VKDEV, rt->color.view, nullptr);
        vkDestroyImage(VKDEV, rt->color.image, nullptr);
        vkFreeMemory(VKDEV, rt->color.memory, nullptr);
        delete rt;
    }
};

// --- PipelineDesc equality (declared in the header) -------------------------
bool PipelineDesc::operator==(const PipelineDesc& o) const {
    return topology == o.topology &&
           vertexLayout.stride == o.vertexLayout.stride &&
           vertexLayout.attribs == o.vertexLayout.attribs &&
           blendEnable == o.blendEnable &&
           srcBlendFactor == o.srcBlendFactor &&
           dstBlendFactor == o.dstBlendFactor &&
           blendOp == o.blendOp &&
           logicOpEnable == o.logicOpEnable &&
           logicOp == o.logicOp &&
           dstAlphaEnable == o.dstAlphaEnable &&
           dstAlphaValue == o.dstAlphaValue &&
           depthTest == o.depthTest &&
           depthWrite == o.depthWrite &&
           depthCompare == o.depthCompare &&
           cullMode == o.cullMode &&
           colorWrite == o.colorWrite &&
           alphaWrite == o.alphaWrite &&
           colorFormat == o.colorFormat &&
           depthFormat == o.depthFormat &&
           textureCount == o.textureCount &&
           fragmentUbo == o.fragmentUbo &&
           vertSpv == o.vertSpv && vertSpvSize == o.vertSpvSize &&
           fragSpv == o.fragSpv && fragSpvSize == o.fragSpvSize;
}

// --- debug labels -----------------------------------------------------------

void Renderer::setDebugName(uint32_t objectType, uint64_t handle, const char* name) {
    if (!mValidation || !name) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = static_cast<VkObjectType>(objectType);
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(VKDEV, &info);
}

// --- lifecycle --------------------------------------------------------------

Renderer& Renderer::instance() {
    static Renderer sInstance;
    return sInstance;
}

bool Renderer::init(SDL_Window* window, const RendererConfig& config) {
    Renderer& r = instance();
    if (r.isInitialized()) {
        return true;
    }
    r.mWindow = window;
    r.mValidation = config.enableValidation;
    r.mVsync = config.vsync;
    r.mClearR = 0.1f;
    r.mClearG = 0.1f;
    r.mClearB = 0.15f;
    r.mClearA = 1.0f;

    if (volkInitialize() != VK_SUCCESS) {
        PL_LOG_FATAL("renderer", "volkInitialize failed (no Vulkan loader on this system?)");
        return false;
    }

    // --- instance -----------------------------------------------------------
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = config.appName ? config.appName : "galaxy-pc";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "galaxy-pc";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions;
    uint32_t sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) {
        PL_LOG_FATAL("renderer", "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }
    extensions.assign(sdlExts, sdlExts + sdlExtCount);
    if (r.mValidation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    std::vector<const char*> layers;
    bool layerEnabled = false;
    if (r.mValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> available(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, available.data());
        for (const auto& l : available) {
            if (std::strcmp(l.layerName, kValidationLayer) == 0) {
                layerEnabled = true;
                break;
            }
        }
        if (layerEnabled) {
            layers.push_back(kValidationLayer);
            PL_LOG_INFO("renderer", "validation layer enabled");
        } else {
            PL_LOG_WARN("renderer", "VK_LAYER_KHRONOS_validation requested but not installed");
        }
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ci, nullptr, reinterpret_cast<VkInstance*>(&r.mInstance)) != VK_SUCCESS) {
        PL_LOG_FATAL("renderer", "vkCreateInstance failed");
        return false;
    }
    volkLoadInstance(VKINST);

    if (r.mValidation && layerEnabled) {
        VkDebugUtilsMessengerCreateInfoEXT dbg{};
        dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = debugMessengerCallback;
        vkCreateDebugUtilsMessengerEXT(VKINST, &dbg, nullptr,
                                       reinterpret_cast<VkDebugUtilsMessengerEXT*>(&r.mDebugMessenger));
    }

    if (!SDL_Vulkan_CreateSurface(window, VKINST, nullptr, reinterpret_cast<VkSurfaceKHR*>(&r.mSurface))) {
        PL_LOG_FATAL("renderer", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    // --- physical + logical device ------------------------------------------
    r.mPhysicalDevice = pickPhysicalDevice(VKINST, VKSURF, &r.mGraphicsFamily, &r.mPresentFamily);

    VkPhysicalDeviceDynamicRenderingFeatures dynRendering{};
    dynRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynRendering.dynamicRendering = VK_TRUE;

    // VK_KHR_swapchain is required for WSI (surface/present). Without it,
    // vkGetDeviceProcAddr returns NULL for the swapchain entry points and
    // volk leaves them null (crash on first vkCreateSwapchainKHR call).
    std::vector<const char*> deviceExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    };
    // VK_EXT_memory_budget is optional (frame stats): enable when available.
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(VKPHYS, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(VKPHYS, nullptr, &extCount, available.data());
        for (const auto& e : available) {
            if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                deviceExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                r.mHasMemoryBudget = true;
                break;
            }
        }
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qInfo{};
    qInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qInfo.queueFamilyIndex = r.mGraphicsFamily;
    qInfo.queueCount = 1;
    qInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qInfo;
    dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    dci.ppEnabledExtensionNames = deviceExts.data();
    dci.pNext = &dynRendering;
    if (vkCreateDevice(VKPHYS, &dci, nullptr, reinterpret_cast<VkDevice*>(&r.mDevice)) != VK_SUCCESS) {
        PL_LOG_FATAL("renderer", "vkCreateDevice failed");
        return false;
    }
    volkLoadDevice(VKDEV);
    vkGetDeviceQueue(VKDEV, r.mGraphicsFamily, 0, reinterpret_cast<VkQueue*>(&r.mGraphicsQueue));
    vkGetDeviceQueue(VKDEV, r.mPresentFamily, 0, reinterpret_cast<VkQueue*>(&r.mPresentQueue));

    // --- sync objects --------------------------------------------------------
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(VKDEV, &fenceInfo, nullptr, reinterpret_cast<VkFence*>(&r.mFence));

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(VKDEV, &semInfo, nullptr, reinterpret_cast<VkSemaphore*>(&r.mImageAvailable));

    // --- command pool (persistent; one command buffer, reset per frame) ------
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(VKDEV, &poolInfo, nullptr, reinterpret_cast<VkCommandPool*>(&r.mCommandPool));

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = reinterpret_cast<VkCommandPool>(r.mCommandPool);
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(VKDEV, &cbAlloc, reinterpret_cast<VkCommandBuffer*>(&r.mCmd));

    // --- GPU timestamps (M4.3 frame stats) ------------------------------------
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(VKPHYS, &props);
        r.mTimestampPeriod = props.limits.timestampPeriod;
        r.mHasTimestamps = props.limits.timestampComputeAndGraphics == VK_TRUE;
        if (r.mHasTimestamps) {
            VkQueryPoolCreateInfo qpi{};
            qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 2; // 0 = frame start, 1 = frame end
            vkCreateQueryPool(VKDEV, &qpi, nullptr, reinterpret_cast<VkQueryPool*>(&r.mQueryPool));
            PL_LOG_INFO("renderer", "GPU timestamps enabled (period %.2f ns)", r.mTimestampPeriod);
        } else {
            PL_LOG_INFO("renderer", "GPU timestamps unsupported (frame stats gpu=0)");
        }
    }

    // --- descriptor pool (textured pipelines M4.2 + TEV UBO M5.4) ------------
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 512;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[1].descriptorCount = 64;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 128;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(VKDEV, &dpci, nullptr, reinterpret_cast<VkDescriptorPool*>(&r.mDescriptorPool));

    // --- M5.4: per-frame fragment-UBO arena (host-visible, dynamic offsets) --
    // One region per draw; endFrame() resets the cursor after the frame fence.
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(VKPHYS, &props);
        r.mUboAlign = props.limits.minUniformBufferOffsetAlignment;
        if (r.mUboAlign < 256) {
            r.mUboAlign = 256;
        }
        // The TEV UBO is 1232 B; round each region up to the alignment.
        r.mUboStride = ((kTevUboRegionBytes + r.mUboAlign - 1) / r.mUboAlign) * r.mUboAlign;
        r.mUboSize = kTevUboArenaBytes;

        VkBufferCreateInfo ubci{};
        ubci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ubci.size = r.mUboSize;
        ubci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VkBuffer uboBuf = VK_NULL_HANDLE;
        vkCreateBuffer(VKDEV, &ubci, nullptr, &uboBuf);
        VkMemoryRequirements uboReq;
        vkGetBufferMemoryRequirements(VKDEV, uboBuf, &uboReq);
        const uint32_t uboMt = findMemoryType(VKPHYS, uboReq.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo umai{};
        umai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        umai.allocationSize = uboReq.size;
        umai.memoryTypeIndex = uboMt;
        VkDeviceMemory uboMem = VK_NULL_HANDLE;
        if (uboMt != UINT32_MAX &&
            vkAllocateMemory(VKDEV, &umai, nullptr, &uboMem) == VK_SUCCESS &&
            vkBindBufferMemory(VKDEV, uboBuf, uboMem, 0) == VK_SUCCESS) {
            r.mUboBuffer = uboBuf;
            r.mUboMemory = uboMem;
            vkMapMemory(VKDEV, uboMem, 0, r.mUboSize, 0,
                        reinterpret_cast<void**>(&r.mUboMapped));
        } else {
            PL_LOG_ERROR("renderer", "fragment UBO arena allocation failed");
            if (uboBuf) vkDestroyBuffer(VKDEV, uboBuf, nullptr);
            if (uboMem) vkFreeMemory(VKDEV, uboMem, nullptr);
            r.mUboBuffer = r.mUboMemory = nullptr;
        }
    }

    // --- swapchain (format decides the pass color format) --------------------
    r.recreateSwapchainInternal();
    if (r.mSwapchain == VK_NULL_HANDLE) {
        return false;
    }
    return true;
}

void Renderer::shutdown() {
    Renderer& r = instance();
    if (!r.isInitialized()) {
        return;
    }
    vkDeviceWaitIdle(VKDEV);

    // Cached pipelines (layouts + descriptor sets are per-entry).
    for (auto& [hash, entry] : r.mPipelineCache) {
        (void)hash;
        if (entry.pipeline) vkDestroyPipeline(VKDEV, reinterpret_cast<VkPipeline>(entry.pipeline), nullptr);
        if (entry.descriptorSetLayout) {
            vkDestroyDescriptorSetLayout(VKDEV, reinterpret_cast<VkDescriptorSetLayout>(entry.descriptorSetLayout), nullptr);
        }
        if (entry.uboSetLayout) {
            vkDestroyDescriptorSetLayout(VKDEV, reinterpret_cast<VkDescriptorSetLayout>(entry.uboSetLayout), nullptr);
        }
        if (entry.layout) vkDestroyPipelineLayout(VKDEV, reinterpret_cast<VkPipelineLayout>(entry.layout), nullptr);
    }
    r.mPipelineCache.clear();
    r.mPipelineByHandle.clear();

    // Sampler cache.
    for (auto& [hash, entry] : r.mSamplerCache) {
        (void)hash;
        vkDestroySampler(VKDEV, reinterpret_cast<VkSampler>(entry.sampler), nullptr);
    }
    r.mSamplerCache.clear();

    if (r.mDescriptorPool) {
        vkDestroyDescriptorPool(VKDEV, reinterpret_cast<VkDescriptorPool>(r.mDescriptorPool), nullptr);
        r.mDescriptorPool = nullptr;
    }
    if (r.mUboMapped) {
        vkUnmapMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(r.mUboMemory));
        r.mUboMapped = nullptr;
    }
    if (r.mUboMemory) {
        vkFreeMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(r.mUboMemory), nullptr);
        r.mUboMemory = nullptr;
    }
    if (r.mUboBuffer) {
        vkDestroyBuffer(VKDEV, reinterpret_cast<VkBuffer>(r.mUboBuffer), nullptr);
        r.mUboBuffer = nullptr;
    }
    if (r.mCmd) {
        vkFreeCommandBuffers(VKDEV, reinterpret_cast<VkCommandPool>(r.mCommandPool), 1,
                             reinterpret_cast<VkCommandBuffer*>(&r.mCmd));
        r.mCmd = nullptr;
    }
    if (r.mCommandPool) {
        vkDestroyCommandPool(VKDEV, reinterpret_cast<VkCommandPool>(r.mCommandPool), nullptr);
        r.mCommandPool = nullptr;
    }
    if (r.mFence) {
        vkDestroyFence(VKDEV, reinterpret_cast<VkFence>(r.mFence), nullptr);
        r.mFence = nullptr;
    }
    if (r.mImageAvailable) {
        vkDestroySemaphore(VKDEV, reinterpret_cast<VkSemaphore>(r.mImageAvailable), nullptr);
        r.mImageAvailable = nullptr;
    }
    if (r.mQueryPool) {
        vkDestroyQueryPool(VKDEV, reinterpret_cast<VkQueryPool>(r.mQueryPool), nullptr);
        r.mQueryPool = nullptr;
    }
    for (const RetiredBuffer& rb : r.mRetiredBuffers) {
        vkDestroyBuffer(VKDEV, reinterpret_cast<VkBuffer>(rb.buffer), nullptr);
        vkFreeMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(rb.memory), nullptr);
    }
    r.mRetiredBuffers.clear();
    // M9.4: retired render targets (the device wait above covers them).
    for (void* rt : r.mRetiredRenderTargets) {
        RendererAccess::destroyRenderTargetNow(rt);
    }
    r.mRetiredRenderTargets.clear();
    r.destroySwapchainInternal();
    vkDestroyDevice(VKDEV, nullptr);
    r.mDevice = nullptr;

    if (r.mDebugMessenger) {
        vkDestroyDebugUtilsMessengerEXT(reinterpret_cast<VkInstance>(r.mInstance),
                                        reinterpret_cast<VkDebugUtilsMessengerEXT>(r.mDebugMessenger),
                                        nullptr);
        r.mDebugMessenger = nullptr;
    }
    if (r.mSurface) {
        vkDestroySurfaceKHR(reinterpret_cast<VkInstance>(r.mInstance),
                            reinterpret_cast<VkSurfaceKHR>(r.mSurface), nullptr);
        r.mSurface = nullptr;
    }
    vkDestroyInstance(reinterpret_cast<VkInstance>(r.mInstance), nullptr);
    r.mInstance = nullptr;
    SDL_Vulkan_UnloadLibrary();
}

// --- swapchain ---------------------------------------------------------------

void Renderer::destroySwapchainInternal() {
    if (mDevice == nullptr) {
        return;
    }
    vkDeviceWaitIdle(VKDEV);
    for (void* sem : mRenderFinished) {
        if (sem) {
            vkDestroySemaphore(VKDEV, reinterpret_cast<VkSemaphore>(sem), nullptr);
        }
    }
    mRenderFinished.clear();
    if (mSwapchain) {
        vkDestroySwapchainKHR(VKDEV, VKSWAP, nullptr);
        mSwapchain = nullptr;
    }
}

void Renderer::recreateSwapchainInternal() {
    SDL_Window* window = reinterpret_cast<SDL_Window*>(mWindow);
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w == 0 || h == 0) {
        PL_LOG_DEBUG("renderer", "window minimized, skipping swapchain recreate");
        return;
    }

    destroySwapchainInternal();

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(VKPHYS, VKSURF, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(VKPHYS, VKSURF, &fmtCount, formats.data());
    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }
    mSwapchainFormat = surfaceFormat.format;

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!mVsync) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(VKPHYS, VKSURF, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(VKPHYS, VKSURF, &modeCount, modes.data());
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                presentMode = m;
                break;
            }
        }
    }

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VKPHYS, VKSURF, &caps);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = VKSURF;
    sci.minImageCount = imageCount;
    sci.imageFormat = surfaceFormat.format;
    sci.imageColorSpace = surfaceFormat.colorSpace;
    sci.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    sci.imageArrayLayers = 1;
    // The EFB→screen blit (blitPassToSwapchain) needs TRANSFER_DST on the
    // swapchain images. Honour the reported surface capabilities.
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    sci.imageSharingMode = (mGraphicsFamily == mPresentFamily) ? VK_SHARING_MODE_EXCLUSIVE
                                                               : VK_SHARING_MODE_CONCURRENT;
    sci.queueFamilyIndexCount = (mGraphicsFamily == mPresentFamily) ? 0 : 2;
    uint32_t families[2] = {mGraphicsFamily, mPresentFamily};
    sci.pQueueFamilyIndices = (mGraphicsFamily == mPresentFamily) ? nullptr : families;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(VKDEV, &sci, nullptr, reinterpret_cast<VkSwapchainKHR*>(&mSwapchain)) != VK_SUCCESS) {
        PL_LOG_FATAL("renderer", "vkCreateSwapchainKHR failed");
        return;
    }
    vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, nullptr);

    // Per-image render-finished semaphores (see Renderer.h member comment).
    mRenderFinished.resize(mImageCount);
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < mImageCount; ++i) {
        vkCreateSemaphore(VKDEV, &semInfo, nullptr,
                          reinterpret_cast<VkSemaphore*>(&mRenderFinished[i]));
    }

    mExtentW = static_cast<uint32_t>(w);
    mExtentH = static_cast<uint32_t>(h);

    PL_LOG_INFO("renderer", "swapchain: %ux%u, %u images, format %d, present %d",
                mExtentW, mExtentH, mImageCount, static_cast<int>(surfaceFormat.format),
                static_cast<int>(presentMode));
}

void Renderer::onResize(int width, int height) {
    (void)width;
    (void)height;
    if (!isInitialized() || mSwapchain == VK_NULL_HANDLE) {
        return;
    }
    recreateSwapchainInternal();
}

// --- frame ------------------------------------------------------------------

bool Renderer::beginFrame() {
    if (!isInitialized() || mSwapchain == VK_NULL_HANDLE) {
        return false;
    }
    if (mSwapchainOutOfDate) {
        mSwapchainOutOfDate = false;
        recreateSwapchainInternal();
        return false;
    }
    // One frame in flight: wait for the previous frame's fence.
    if (vkWaitForFences(VKDEV, 1, reinterpret_cast<VkFence*>(&mFence), VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }
    vkResetFences(VKDEV, 1, reinterpret_cast<VkFence*>(&mFence));

    VkResult acquire = vkAcquireNextImageKHR(VKDEV, VKSWAP, UINT64_MAX,
                                             reinterpret_cast<VkSemaphore>(mImageAvailable),
                                             VK_NULL_HANDLE, &mImageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        mSwapchainOutOfDate = true;
        return false;
    }
    if (acquire == VK_SUBOPTIMAL_KHR) {
        mSwapchainOutOfDate = true;
    }
    mFrameRecording = true;
    mFrameAcquireConsumed = false; // flushFrame() consumes it if called
    mSwapPresentReady = false;     // fresh swap image: nothing presented yet

    // Begin recording (persistent command buffer, reset each frame).
    vkResetCommandPool(VKDEV, reinterpret_cast<VkCommandPool>(mCommandPool), 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(reinterpret_cast<VkCommandBuffer>(mCmd), &bi);

    // Frame stats: CPU phase starts after acquire; GPU frame timestamps.
    mCpuPhaseStart = Platform::Timing::nowSeconds();
    if (mHasTimestamps && mQueryPool) {
        VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
        vkCmdResetQueryPool(cmd, reinterpret_cast<VkQueryPool>(mQueryPool), 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            reinterpret_cast<VkQueryPool>(mQueryPool), 0);
    }
    return true;
}

void Renderer::setClearColor(float r, float g, float b, float a) {
    mClearR = r;
    mClearG = g;
    mClearB = b;
    mClearA = a;
}

void Renderer::recordBeginPass(void* colorViewIn, void* depthViewIn, bool hasDepth,
                               uint32_t w, uint32_t h, const ClearValue& clear) {
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
    VkImageView colorView = reinterpret_cast<VkImageView>(colorViewIn);
    VkImageView depthView = reinterpret_cast<VkImageView>(depthViewIn);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = colorView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = {clear.r, clear.g, clear.b, clear.a};

    VkRenderingAttachmentInfo depthAttachment{};
    if (hasDepth && depthView) {
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depthView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = {clear.depth, clear.stencil};
    }

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, {w, h}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    if (hasDepth && depthView) {
        renderingInfo.pDepthAttachment = &depthAttachment;
    }
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    mInPass = true;
}

void Renderer::beginPass() {
    ClearValue clear;
    clear.r = mClearR;
    clear.g = mClearG;
    clear.b = mClearB;
    clear.a = mClearA;
    beginPass(clear);
}

void Renderer::beginPass(const ClearValue& clear) {
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);

    std::vector<VkImage> images(mImageCount);
    vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, images.data());
    const VkImage swapImage = images[mImageIndex];

    // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (we clear everything anyway).
    VkImageMemoryBarrier toAttachment{};
    toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = swapImage;
    toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toAttachment.srcAccessMask = 0;
    toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttachment);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = swapImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = static_cast<VkFormat>(mSwapchainFormat);
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (mPassImageView) {
        vkDestroyImageView(VKDEV, reinterpret_cast<VkImageView>(mPassImageView), nullptr);
        mPassImageView = nullptr;
    }
    vkCreateImageView(VKDEV, &vci, nullptr, reinterpret_cast<VkImageView*>(&mPassImageView));

    mPassTarget = nullptr; // swapchain pass
    recordBeginPass(reinterpret_cast<VkImageView>(mPassImageView), VK_NULL_HANDLE, false,
                    mExtentW, mExtentH, clear);
}

void Renderer::beginPass(RenderTargetHandle target) {
    ClearValue clear;
    clear.r = mClearR;
    clear.g = mClearG;
    clear.b = mClearB;
    clear.a = mClearA;
    beginPass(target, clear);
}

void Renderer::beginPass(RenderTargetHandle target, const ClearValue& clear) {
    auto* rt = reinterpret_cast<GpuRenderTarget*>(target);
    if (!rt) {
        return;
    }
    // The RT color image may be left in TRANSFER_SRC_OPTIMAL by a previous
    // blitPassToSwapchain/readRenderTarget. The pass clears the whole image
    // (LOAD_OP_CLEAR), so transition UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    // (contents discarded) instead of tracking the last layout.
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
    VkImageMemoryBarrier toColor{};
    toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = rt->color.image;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toColor.srcAccessMask = 0;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toColor);
    rt->colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    mPassTarget = target;
    recordBeginPass(rt->color.view, rt->hasDepth ? rt->depth.view : VK_NULL_HANDLE,
                    rt->hasDepth, rt->color.width, rt->color.height, clear);
}

void Renderer::endPass() {
    if (!mInPass) {
        return;
    }
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
    vkCmdEndRendering(cmd);

    if (mPassTarget == nullptr) {
        // Swapchain: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR.
        std::vector<VkImage> images(mImageCount);
        vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, images.data());
        const VkImage swapImage = images[mImageIndex];
        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = swapImage;
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toPresent);
        mSwapPresentReady = true;
    }
    // Render target: leave the color in COLOR_ATTACHMENT_OPTIMAL (readback /
    // sampling transitions are done by the caller — M5's GXCopyDisp blit).
    mInPass = false;
}

void Renderer::endFrame() {
    if (mInPass) {
        endPass();
    }
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
    if (!mSwapPresentReady) {
        // Empty/aborted frame (e.g. a scene transition where the game opened
        // no pass or never blitted): present still happens, so transition the
        // untouched swap image UNDEFINED -> PRESENT_SRC (contents discarded).
        std::vector<VkImage> images(mImageCount);
        vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, images.data());
        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = images[mImageIndex];
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toPresent.srcAccessMask = 0;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toPresent);
        mSwapPresentReady = true;
    }
    if (mHasTimestamps && mQueryPool) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            reinterpret_cast<VkQueryPool>(mQueryPool), 1);
    }
    vkEndCommandBuffer(cmd);

    VkSemaphore imageAvailable = reinterpret_cast<VkSemaphore>(mImageAvailable);
    // Semaphore owned by this swapchain image (never reused while the previous
    // present of that image is still in flight).
    VkSemaphore renderFinished = reinterpret_cast<VkSemaphore>(mRenderFinished[mImageIndex]);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    // NOTE: waitStage must outlive vkQueueSubmit — declaring it inside the if
    // block left pWaitDstStageMask dangling (UB, caught by validation layers).
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (!mFrameAcquireConsumed) {
        // flushFrame() (M5.7c) already waited on the acquire semaphore; only
        // the first submission of the frame may wait on it.
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
    }
    vkQueueSubmit(VKGQ, 1, &submit, reinterpret_cast<VkFence>(mFence));
    mFrameRecording = false;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    VkSwapchainKHR swapchains[1] = {VKSWAP};
    present.pSwapchains = swapchains;
    present.pImageIndices = &mImageIndex;
    const VkResult presentResult = vkQueuePresentKHR(VKPQ, &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        mSwapchainOutOfDate = true;
    }

    // One frame in flight: wait for the submit before destroying the transient
    // image view and resetting the pool next frame (nothing may be pending).
    vkWaitForFences(VKDEV, 1, reinterpret_cast<VkFence*>(&mFence), VK_TRUE, UINT64_MAX);
    if (mPassImageView) {
        vkDestroyImageView(VKDEV, reinterpret_cast<VkImageView>(mPassImageView), nullptr);
        mPassImageView = nullptr;
    }

    // M5.4: the fragment-UBO arena is single-buffered; safe to rewind now that
    // the fence guarantees the GPU finished reading every region.
    mUboCursor = 0;

    // M5.2: destroy dynamic-buffer allocations retired during this frame (the
    // fence above guarantees no command buffer references them anymore).
    for (const RetiredBuffer& rb : mRetiredBuffers) {
        vkDestroyBuffer(VKDEV, reinterpret_cast<VkBuffer>(rb.buffer), nullptr);
        vkFreeMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(rb.memory), nullptr);
    }
    mRetiredBuffers.clear();

    // M9.4: destroy render targets retired during this frame (same fence
    // guarantee — see destroyRenderTarget).
    for (void* rt : mRetiredRenderTargets) {
        RendererAccess::destroyRenderTargetNow(rt);
    }
    mRetiredRenderTargets.clear();

    // --- frame statistics (M4.3) -------------------------------------------
    if (mCpuPhaseStart > 0.0) {
        mLastFrameStats.cpuRenderMs = (Platform::Timing::nowSeconds() - mCpuPhaseStart) * 1000.0;
        mCpuPhaseStart = 0.0;
    }
    if (mHasTimestamps && mQueryPool) {
        uint64_t ticks[2] = {0, 0};
        if (vkGetQueryPoolResults(VKDEV, reinterpret_cast<VkQueryPool>(mQueryPool), 0, 2,
                                  sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            mLastFrameStats.gpuMs = gpuTicksToMs(ticks[1] - ticks[0], mTimestampPeriod);
        }
    }
    updateMemoryBudget();
}

void Renderer::waitIdle() {
    if (mDevice) {
        vkDeviceWaitIdle(VKDEV);
    }
}

void Renderer::updateMemoryBudget() {
    if (!mHasMemoryBudget) {
        mLastFrameStats.memoryUsed = 0;
        mLastFrameStats.memoryBudget = 0;
        return;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    props2.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(VKPHYS, &props2);

    // Report the first heap that has a device-local memory type (the GPU VRAM).
    for (uint32_t i = 0; i < props2.memoryProperties.memoryHeapCount; ++i) {
        bool deviceLocal = false;
        for (uint32_t t = 0; t < props2.memoryProperties.memoryTypeCount; ++t) {
            if (props2.memoryProperties.memoryTypes[t].heapIndex == i &&
                (props2.memoryProperties.memoryTypes[t].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                deviceLocal = true;
                break;
            }
        }
        if (deviceLocal) {
            mLastFrameStats.memoryUsed = budget.heapUsage[i];
            mLastFrameStats.memoryBudget = budget.heapBudget[i];
            return;
        }
    }
    mLastFrameStats.memoryUsed = 0;
    mLastFrameStats.memoryBudget = 0;
}

VertexFormat Renderer::passColorFormat() const {
    switch (mSwapchainFormat) {
        case VK_FORMAT_B8G8R8A8_UNORM: return VertexFormat::B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return VertexFormat::R8G8B8A8_UNORM;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return VertexFormat::R32G32B32A32_SFLOAT;
        default:
            PL_LOG_FATAL("renderer", "unsupported swapchain color format %d", mSwapchainFormat);
            return VertexFormat::R8G8B8A8_UNORM;
    }
}

TextureFormat Renderer::passDepthFormat() const {
    if (mPassTarget == nullptr) {
        return TextureFormat::Undefined;  // swapchain pass: no depth attachment
    }
    const GpuRenderTarget* rt = reinterpret_cast<const GpuRenderTarget*>(mPassTarget);
    return rt->hasDepth ? rt->depth.format : TextureFormat::Undefined;
}

// --- pass state -------------------------------------------------------------

void Renderer::setViewport(float x, float y, float w, float h) {
    if (!mFrameRecording) {
        // vkCmdSetViewport requires a recording command buffer. Calls outside
        // a host frame (e.g. GXSetViewport before beginRender) are dropped
        // here; GXCompat mirrors them and re-applies in flushDraw.
        return;
    }
    VkViewport vp{x, y, w, h, 0.0f, 1.0f};
    vkCmdSetViewport(reinterpret_cast<VkCommandBuffer>(mCmd), 0, 1, &vp);
}

void Renderer::setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!mFrameRecording) {
        return; // see setViewport
    }
    VkRect2D scissor{{static_cast<int32_t>(x), static_cast<int32_t>(y)}, {w, h}};
    vkCmdSetScissor(reinterpret_cast<VkCommandBuffer>(mCmd), 0, 1, &scissor);
}

// --- buffers ----------------------------------------------------------------

BufferHandle Renderer::createBuffer(BufferUsage usage, uint64_t size, const void* initialData) {
    VkBufferUsageFlags vkUsage = 0;
    switch (usage) {
        case BufferUsage::Vertex: vkUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
        case BufferUsage::Index: vkUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
        case BufferUsage::Uniform: vkUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        case BufferUsage::Staging: vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; break;
    }

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = vkUsage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(VKDEV, &bci, nullptr, &buffer) != VK_SUCCESS) {
        PL_LOG_ERROR("renderer", "vkCreateBuffer failed (size %llu)", static_cast<unsigned long long>(size));
        return nullptr;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(VKDEV, buffer, &memReq);
    VkDeviceMemory memory = RendererAccess::allocateMemory(memReq.size, memReq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!memory) {
        PL_LOG_ERROR("renderer", "no host-visible+coherent memory for buffer");
        vkDestroyBuffer(VKDEV, buffer, nullptr);
        return nullptr;
    }
    vkBindBufferMemory(VKDEV, buffer, memory, 0);

    if (initialData) {
        void* mapped = nullptr;
        vkMapMemory(VKDEV, memory, 0, size, 0, &mapped);
        std::memcpy(mapped, initialData, static_cast<size_t>(size));
        vkUnmapMemory(VKDEV, memory);
    }

    auto* out = new GpuBuffer{buffer, memory, size};
    return reinterpret_cast<BufferHandle>(out);
}

// --- dynamic buffer (M5.2) ---------------------------------------------------

BufferHandle Renderer::createDynamicBuffer(uint64_t initialSize) {
    const uint64_t size = std::max<uint64_t>(initialSize, 1024);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(VKDEV, &bci, nullptr, &buffer) != VK_SUCCESS) {
        PL_LOG_ERROR("renderer", "vkCreateBuffer (dynamic) failed (size %llu)",
                     static_cast<unsigned long long>(size));
        return nullptr;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(VKDEV, buffer, &memReq);
    VkDeviceMemory memory = RendererAccess::allocateMemory(
        memReq.size, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!memory) {
        PL_LOG_ERROR("renderer", "no host-visible+coherent memory for dynamic buffer");
        vkDestroyBuffer(VKDEV, buffer, nullptr);
        return nullptr;
    }
    vkBindBufferMemory(VKDEV, buffer, memory, 0);

    void* mapped = nullptr;
    vkMapMemory(VKDEV, memory, 0, VK_WHOLE_SIZE, 0, &mapped);

    auto* out = new GpuBuffer{buffer, memory, size, mapped, size};
    return reinterpret_cast<BufferHandle>(out);
}

bool Renderer::ensureBufferCapacity(BufferHandle handle, uint64_t minBytes) {
    auto* gpu = reinterpret_cast<GpuBuffer*>(handle);
    if (!gpu || !gpu->mapped) {
        return false;
    }
    if (minBytes <= gpu->capacity) {
        return true;
    }
    // Grow: keep the old allocation alive until the frame fence (draws already
    // recorded this frame reference it) — it goes to the retire queue and is
    // destroyed at the next endFrame().
    const uint64_t newSize = std::max(minBytes, gpu->capacity * 2);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = newSize;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer newBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(VKDEV, &bci, nullptr, &newBuffer) != VK_SUCCESS) {
        PL_LOG_ERROR("renderer", "vkCreateBuffer (dynamic grow) failed (size %llu)",
                     static_cast<unsigned long long>(newSize));
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(VKDEV, newBuffer, &memReq);
    VkDeviceMemory newMemory = RendererAccess::allocateMemory(
        memReq.size, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!newMemory) {
        PL_LOG_ERROR("renderer", "no host-visible+coherent memory for dynamic grow");
        vkDestroyBuffer(VKDEV, newBuffer, nullptr);
        return false;
    }
    vkBindBufferMemory(VKDEV, newBuffer, newMemory, 0);
    void* newMapped = nullptr;
    vkMapMemory(VKDEV, newMemory, 0, VK_WHOLE_SIZE, 0, &newMapped);

    // Preserve existing contents (host memory to host memory).
    std::memcpy(newMapped, gpu->mapped, static_cast<size_t>(std::min(gpu->size, newSize)));

    mRetiredBuffers.push_back({gpu->buffer, gpu->memory});
    gpu->buffer = newBuffer;
    gpu->memory = newMemory;
    gpu->mapped = newMapped;
    gpu->size = newSize;
    gpu->capacity = newSize;
    return true;
}

bool Renderer::updateDynamicBuffer(BufferHandle handle, uint64_t offset, uint64_t size,
                                   const void* data) {
    auto* gpu = reinterpret_cast<GpuBuffer*>(handle);
    if (!gpu || !gpu->mapped || !data) {
        return false;
    }
    if (offset + size > gpu->capacity) {
        PL_LOG_ERROR("renderer", "updateDynamicBuffer out of bounds (off %llu size %llu cap %llu)",
                     static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(gpu->capacity));
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(gpu->mapped) + offset, data, static_cast<size_t>(size));
    return true;
}

void Renderer::destroyBuffer(BufferHandle buffer) {
    if (!buffer) {
        return;
    }
    auto* gpu = reinterpret_cast<GpuBuffer*>(buffer);
    vkDestroyBuffer(VKDEV, gpu->buffer, nullptr);
    vkFreeMemory(VKDEV, gpu->memory, nullptr);
    delete gpu;
}

void Renderer::bindVertexBuffer(BufferHandle buffer, uint64_t offset) {
    auto* gpu = reinterpret_cast<GpuBuffer*>(buffer);
    VkBuffer vb = gpu->buffer;
    VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(reinterpret_cast<VkCommandBuffer>(mCmd), 0, 1, &vb, &off);
}

void Renderer::bindIndexBuffer(BufferHandle buffer, uint64_t offset) {
    auto* gpu = reinterpret_cast<GpuBuffer*>(buffer);
    vkCmdBindIndexBuffer(reinterpret_cast<VkCommandBuffer>(mCmd), gpu->buffer, offset,
                         VK_INDEX_TYPE_UINT32);
}

// --- textures / samplers / render targets (M4.2) ----------------------------

TextureHandle Renderer::createTexture(const TextureDesc& desc) {
    const VkFormat vkFormat = textureFormatToVk(desc.format);
    if (vkFormat == VK_FORMAT_UNDEFINED) {
        PL_LOG_ERROR("renderer", "createTexture: unsupported format %d", static_cast<int>(desc.format));
        return nullptr;
    }
    const bool hasData = desc.initialData != nullptr;
    const VkDeviceSize dataBytes = static_cast<VkDeviceSize>(desc.width) * desc.height * 4;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkFormat;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1; // M4.2: no mip chain (flag reserved for later)
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(VKDEV, &ici, nullptr, &image) != VK_SUCCESS) {
        PL_LOG_ERROR("renderer", "createTexture: vkCreateImage failed");
        return nullptr;
    }
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(VKDEV, image, &memReq);
    VkDeviceMemory memory = RendererAccess::allocateMemory(memReq.size, memReq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory) {
        PL_LOG_ERROR("renderer", "createTexture: no device-local memory");
        vkDestroyImage(VKDEV, image, nullptr);
        return nullptr;
    }
    vkBindImageMemory(VKDEV, image, memory, 0);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vkFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(VKDEV, &vci, nullptr, &view);

    auto* tex = new GpuTexture{image, memory, view, desc.width, desc.height, desc.format};
    setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image),
                 desc.debugName ? desc.debugName : "texture");

    if (hasData) {
        // Upload via a one-time command buffer with a host-visible staging
        // buffer (simple and correct; a persistent staging ring comes with
        // heavier streaming in M5+).
        VkBufferCreateInfo sbci{};
        sbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        sbci.size = dataBytes;
        sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        vkCreateBuffer(VKDEV, &sbci, nullptr, &staging);
        VkMemoryRequirements sreq;
        vkGetBufferMemoryRequirements(VKDEV, staging, &sreq);
        VkDeviceMemory stagingMem = RendererAccess::allocateMemory(sreq.size, sreq.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkBindBufferMemory(VKDEV, staging, stagingMem, 0);
        void* mapped = nullptr;
        vkMapMemory(VKDEV, stagingMem, 0, dataBytes, 0, &mapped);
        std::memcpy(mapped, desc.initialData, static_cast<size_t>(dataBytes));
        vkUnmapMemory(VKDEV, stagingMem);

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = reinterpret_cast<VkCommandPool>(mCommandPool);
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(VKDEV, &ai, &cmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = image;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {desc.width, desc.height, 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier toRead{};
        toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(VKGQ, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(VKGQ);
        vkFreeCommandBuffers(VKDEV, reinterpret_cast<VkCommandPool>(mCommandPool), 1, &cmd);
        vkDestroyBuffer(VKDEV, staging, nullptr);
        vkFreeMemory(VKDEV, stagingMem, nullptr);
    } else {
        // Undefined contents; transition so sampling is legal.
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = reinterpret_cast<VkCommandPool>(mCommandPool);
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(VKDEV, &ai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier toRead{};
        toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toRead.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toRead.srcAccessMask = 0;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(VKGQ, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(VKGQ);
        vkFreeCommandBuffers(VKDEV, reinterpret_cast<VkCommandPool>(mCommandPool), 1, &cmd);
    }

    return reinterpret_cast<TextureHandle>(tex);
}

void Renderer::destroyTexture(TextureHandle texture) {
    if (!texture) {
        return;
    }
    auto* tex = reinterpret_cast<GpuTexture*>(texture);
    vkDestroyImageView(VKDEV, tex->view, nullptr);
    vkDestroyImage(VKDEV, tex->image, nullptr);
    vkFreeMemory(VKDEV, tex->memory, nullptr);
    delete tex;
}

SamplerHandle Renderer::getOrCreateSampler(const SamplerDesc& desc) {
    const uint64_t hash = hashSamplerDesc(desc);
    const auto it = mSamplerCache.find(hash);
    if (it != mSamplerCache.end() && it->second.desc == desc) {
        return reinterpret_cast<SamplerHandle>(it->second.sampler);
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = filterToVk(desc.magFilter);
    sci.minFilter = filterToVk(desc.minFilter);
    sci.mipmapMode = mipFilterToVk(desc.mipFilter);
    sci.addressModeU = addressModeToVk(desc.addressU);
    sci.addressModeV = addressModeToVk(desc.addressV);
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.0f;
    sci.minLod = 0.0f;
    // No anisotropy / compare / border in M4.2 (GX has no anisotropic filtering
    // on the real console; aniso may come as an enhancement later).
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(VKDEV, &sci, nullptr, &sampler) != VK_SUCCESS) {
        PL_LOG_ERROR("renderer", "getOrCreateSampler: vkCreateSampler failed");
        return nullptr;
    }
    mSamplerCache[hash] = {desc, reinterpret_cast<void*>(sampler)};
    return reinterpret_cast<SamplerHandle>(sampler);
}

void Renderer::bindTexture(uint32_t binding, TextureHandle texture, SamplerHandle sampler) {
    if (!mBoundEntry || !mBoundEntry->descriptorSet) {
        PL_LOG_ERROR("renderer", "bindTexture: no textured pipeline bound");
        return;
    }
    if (binding >= mBoundEntry->desc.textureCount) {
        PL_LOG_ERROR("renderer", "bindTexture: binding %u out of range (textureCount %u)",
                     binding, mBoundEntry->desc.textureCount);
        return;
    }
    auto* tex = reinterpret_cast<GpuTexture*>(texture);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = reinterpret_cast<VkSampler>(sampler);
    imageInfo.imageView = tex->view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = reinterpret_cast<VkDescriptorSet>(mBoundEntry->descriptorSet);
    write.dstBinding = 0;
    write.dstArrayElement = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(VKDEV, 1, &write, 0, nullptr);

    VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(mBoundEntry->descriptorSet);
    vkCmdBindDescriptorSets(reinterpret_cast<VkCommandBuffer>(mCmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            reinterpret_cast<VkPipelineLayout>(mBoundEntry->layout),
                            0, 1, &set, 0, nullptr);
}

void Renderer::bindFragmentTextures(const TextureHandle* tex, const SamplerHandle* sam,
                                    uint32_t count) {
    if (!mBoundEntry || !mBoundEntry->descriptorSet) {
        PL_LOG_ERROR("renderer", "bindFragmentTextures: no textured pipeline bound");
        return;
    }
    if (count > mBoundEntry->desc.textureCount || !tex || !sam) {
        PL_LOG_ERROR("renderer", "bindFragmentTextures: %u out of range (textureCount %u)",
                     count, mBoundEntry->desc.textureCount);
        return;
    }
    VkWriteDescriptorSet writes[8]{};
    VkDescriptorImageInfo images[8]{};
    for (uint32_t i = 0; i < count; ++i) {
        auto* t = reinterpret_cast<GpuTexture*>(tex[i]);
        images[i].sampler = reinterpret_cast<VkSampler>(sam[i]);
        images[i].imageView = t->view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = reinterpret_cast<VkDescriptorSet>(mBoundEntry->descriptorSet);
        writes[i].dstBinding = 0;
        writes[i].dstArrayElement = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(VKDEV, count, writes, 0, nullptr);

    VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(mBoundEntry->descriptorSet);
    vkCmdBindDescriptorSets(reinterpret_cast<VkCommandBuffer>(mCmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            reinterpret_cast<VkPipelineLayout>(mBoundEntry->layout),
                            0, 1, &set, 0, nullptr);
}

bool Renderer::uploadFragmentUbo(const void* data, uint32_t size) {
    if (!mBoundEntry || !mBoundEntry->uboSet) {
        PL_LOG_ERROR("renderer", "uploadFragmentUbo: no fragmentUbo pipeline bound");
        return false;
    }
    if (!mUboMapped) {
        PL_LOG_ERROR("renderer", "uploadFragmentUbo: UBO arena unavailable");
        return false;
    }
    if (size > mUboStride) {
        PL_LOG_ERROR("renderer", "uploadFragmentUbo: %u bytes exceed region %llu",
                     size, static_cast<unsigned long long>(mUboStride));
        return false;
    }
    if (mUboCursor + mUboStride > mUboSize) {
        PL_LOG_ERROR("renderer", "uploadFragmentUbo: UBO arena exhausted (%llu B/frame) — "
                                 "draw dropped", static_cast<unsigned long long>(mUboSize));
        return false;
    }
    const uint32_t dynamicOffset = static_cast<uint32_t>(mUboCursor);
    std::memcpy(mUboMapped + mUboCursor, data, size);
    mUboCursor += mUboStride;

    VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(mBoundEntry->uboSet);
    vkCmdBindDescriptorSets(reinterpret_cast<VkCommandBuffer>(mCmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            reinterpret_cast<VkPipelineLayout>(mBoundEntry->layout),
                            1, 1, &set, 1, &dynamicOffset);
    return true;
}

RenderTargetHandle Renderer::createRenderTarget(const RenderTargetDesc& desc) {
    auto* rt = new GpuRenderTarget();
    rt->hasDepth = desc.hasDepth;

    // Color image: attach + transfer-src (readback for tests / future blits).
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = textureFormatToVk(desc.colorFormat);
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage colorImage = VK_NULL_HANDLE;
    vkCreateImage(VKDEV, &ici, nullptr, &colorImage);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(VKDEV, colorImage, &memReq);
    VkDeviceMemory colorMem = RendererAccess::allocateMemory(memReq.size, memReq.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(VKDEV, colorImage, colorMem, 0);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = colorImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView colorView = VK_NULL_HANDLE;
    vkCreateImageView(VKDEV, &vci, nullptr, &colorView);

    rt->color = {colorImage, colorMem, colorView, desc.width, desc.height, desc.colorFormat};
    setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(colorImage),
                 desc.debugName ? desc.debugName : "render-target");

    // Transition color UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL once.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = reinterpret_cast<VkCommandPool>(mCommandPool);
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(VKDEV, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toColor{};
    toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = colorImage;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toColor.srcAccessMask = 0;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toColor);

    if (desc.hasDepth) {
        VkImageCreateInfo dici{};
        dici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = textureFormatToVk(desc.depthFormat);
        dici.extent = {desc.width, desc.height, 1};
        dici.mipLevels = 1;
        dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        dici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage depthImage = VK_NULL_HANDLE;
        vkCreateImage(VKDEV, &dici, nullptr, &depthImage);
        VkMemoryRequirements dreq;
        vkGetImageMemoryRequirements(VKDEV, depthImage, &dreq);
        VkDeviceMemory depthMem = RendererAccess::allocateMemory(dreq.size, dreq.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkBindImageMemory(VKDEV, depthImage, depthMem, 0);

        VkImageViewCreateInfo dvci{};
        dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvci.image = depthImage;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = dici.format;
        dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkImageView depthView = VK_NULL_HANDLE;
        vkCreateImageView(VKDEV, &dvci, nullptr, &depthView);

        rt->depth = {depthImage, depthMem, depthView, desc.width, desc.height, desc.depthFormat};
        setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(depthImage),
                     desc.debugName ? desc.debugName : "render-target-depth");

        VkImageMemoryBarrier toDepth{};
        toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDepth.image = depthImage;
        // Combined depth/stencil format without separateDepthStencilLayouts:
        // the barrier must cover BOTH aspects
        // (VUID-VkImageMemoryBarrier-image-03320). The image VIEW above may
        // still expose only the depth aspect (valid for attachments).
        toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        toDepth.srcAccessMask = 0;
        toDepth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDepth);
    }

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(VKGQ, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(VKGQ);
    vkFreeCommandBuffers(VKDEV, reinterpret_cast<VkCommandPool>(mCommandPool), 1, &cmd);

    return reinterpret_cast<RenderTargetHandle>(rt);
}

void Renderer::destroyRenderTarget(RenderTargetHandle target) {
    if (!target) {
        return;
    }
    // M9.4: deferred destruction — the target may still be referenced by the
    // frame being recorded (blitPassToSwapchain reads it through mPassTarget
    // even after endPass) and by already-submitted frames. The retire queue
    // is flushed in endFrame() after the frame fence, and at shutdown.
    mRetiredRenderTargets.push_back(target);
}

// --- M5.7c: EFB copy / present ------------------------------------------------

bool Renderer::blitPassToSwapchain() {
    if (!isInitialized() || !mCmd || mPassTarget == nullptr) {
        // No offscreen pass to blit (the last pass was the swapchain itself):
        // the swap image is already PRESENT_SRC after endPass().
        return false;
    }
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
    auto* rt = reinterpret_cast<GpuRenderTarget*>(mPassTarget);
    const VkImage efbImage = rt->color.image;
    const int32_t efbW = static_cast<int32_t>(rt->color.width);
    const int32_t efbH = static_cast<int32_t>(rt->color.height);

    std::vector<VkImage> images(mImageCount);
    vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, images.data());
    const VkImage swapImage = images[mImageIndex];

    // EFB: <tracked layout> -> TRANSFER_SRC_OPTIMAL. Normally the pass just
    // ended (COLOR_ATTACHMENT_OPTIMAL); use the tracked value so back-to-back
    // blits/readbacks stay validation-clean.
    const bool fromColorAttachment =
        (rt->colorLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageMemoryBarrier efbBarrier{};
    efbBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    efbBarrier.oldLayout = rt->colorLayout;
    efbBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    efbBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    efbBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    efbBarrier.image = efbImage;
    efbBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    efbBarrier.srcAccessMask = fromColorAttachment
                                   ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                   : VK_ACCESS_TRANSFER_READ_BIT;
    efbBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    // Swapchain: UNDEFINED -> TRANSFER_DST_OPTIMAL (the blit overwrites the
    // whole image; contents are discarded).
    VkImageMemoryBarrier swapBarrier{};
    swapBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.image = swapImage;
    swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    swapBarrier.srcAccessMask = 0;
    swapBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier barriers[2] = {efbBarrier, swapBarrier};
    vkCmdPipelineBarrier(cmd,
                         fromColorAttachment
                             ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, barriers);

    // Scale the EFB to the window (the GX XFB was 640x448/576; the window is
    // any size). LINEAR filter = the XFB vertical filter, simplified.
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {efbW, efbH, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {0, 0, 0};
    region.dstOffsets[1] = {static_cast<int32_t>(mExtentW), static_cast<int32_t>(mExtentH), 1};
    vkCmdBlitImage(cmd, efbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);

    // Swapchain: TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR (endFrame submits).
    VkImageMemoryBarrier toPresent{};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = swapImage;
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toPresent);
    mSwapPresentReady = true;

    rt->colorLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    mPassTarget = nullptr; // the blit consumed the pass
    return true;
}

bool Renderer::flushFrame() {
    if (!isInitialized() || !mCmd || !mFrameRecording) {
        return false; // no frame open (or renderer down): nothing to flush
    }
    // The pass (if any) was already ended by the caller (GXCopyTex between
    // passes); a mid-pass flush is not supported — see docs/gx.md §J.
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(mCmd);
    vkEndCommandBuffer(cmd);

    // First submission of the frame: wait on the acquire semaphore (later
    // endFrame() submissions skip it via mFrameAcquireConsumed).
    VkSemaphore imageAvailable = reinterpret_cast<VkSemaphore>(mImageAvailable);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(VKGQ, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(VKGQ); // synchronous: readbacks right after see the content
    mFrameAcquireConsumed = true;

    // Reopen a fresh command buffer for the rest of the frame (the blit in
    // GXCopyDisp + the present at endFrame()). The previous recording is
    // complete (queue idle above), so the buffer can be reset and re-begun.
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    if (mHasTimestamps && mQueryPool) {
        vkCmdResetQueryPool(cmd, reinterpret_cast<VkQueryPool>(mQueryPool), 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            reinterpret_cast<VkQueryPool>(mQueryPool), 0);
    }
    return true;
}

bool Renderer::readRenderTarget(RenderTargetHandle target, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h, void* outRgba8) {
    if (!isInitialized() || !target || !outRgba8 || w == 0 || h == 0) {
        return false;
    }
    auto* rt = reinterpret_cast<GpuRenderTarget*>(target);
    if (x >= rt->color.width || y >= rt->color.height) {
        return false;
    }
    w = std::min(w, rt->color.width - x);
    h = std::min(h, rt->color.height - y);
    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(w) * 4;
    const VkDeviceSize bufBytes = rowBytes * h;

    // Staging buffer: host-visible+coherent, persistently mapped (createBuffer
    // only maps when initialData is given, so build it directly here).
    VkBuffer staging = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(VKDEV, &bci, nullptr, &staging) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements stageReq;
    vkGetBufferMemoryRequirements(VKDEV, staging, &stageReq);
    VkDeviceMemory stageMem = RendererAccess::allocateMemory(
        stageReq.size, stageReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!stageMem) {
        vkDestroyBuffer(VKDEV, staging, nullptr);
        return false;
    }
    vkBindBufferMemory(VKDEV, staging, stageMem, 0);
    void* mapped = nullptr;
    if (vkMapMemory(VKDEV, stageMem, 0, bufBytes, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(VKDEV, stageMem, nullptr);
        vkDestroyBuffer(VKDEV, staging, nullptr);
        return false;
    }

    // One-shot command buffer from the frame pool. Safe to run while the frame
    // command buffer is open: it is submitted + queue-waited below before this
    // function returns, so it never overlaps the frame's execution.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = reinterpret_cast<VkCommandPool>(mCommandPool);
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(VKDEV, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // The RT color is in COLOR_ATTACHMENT_OPTIMAL (created so, restored by
    // beginPass/readRenderTarget). If a pass is currently open on it, the
    // recorded-but-unsubmitted draws are not visible to this readback (the
    // frame submits after the readback) — see docs/gx.md §J.
    // The image may already be in TRANSFER_SRC_OPTIMAL (a previous blit or
    // readback left it there) — use the tracked layout, not a hardcoded one.
    const bool fromColorAttachment =
        (rt->colorLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = rt->colorLayout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = rt->color.image;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = fromColorAttachment
                              ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              : VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         fromColorAttachment
                             ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toSrc);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;   // tightly packed
    copy.bufferImageHeight = 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, rt->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &copy);

    VkImageMemoryBarrier backToColor{};
    backToColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    backToColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    backToColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backToColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backToColor.image = rt->color.image;
    backToColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    backToColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &backToColor);
    rt->colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(VKGQ, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(VKGQ);
    vkFreeCommandBuffers(VKDEV, reinterpret_cast<VkCommandPool>(mCommandPool), 1, &cmd);

    // Convert the raw attachment bytes to R8G8B8A8 (the copy is tightly
    // packed; the attachment may be BGRA).
    const uint8_t* raw = static_cast<const uint8_t*>(mapped);
    uint8_t* out = static_cast<uint8_t*>(outRgba8);
    if (rt->color.format == TextureFormat::B8G8R8A8_UNORM) {
        for (VkDeviceSize r = 0; r < h; ++r) {
            const uint8_t* srcRow = raw + r * rowBytes;
            uint8_t* dstRow = out + r * rowBytes;
            for (VkDeviceSize i = 0; i < rowBytes; i += 4) {
                dstRow[i + 0] = srcRow[i + 2];
                dstRow[i + 1] = srcRow[i + 1];
                dstRow[i + 2] = srcRow[i + 0];
                dstRow[i + 3] = srcRow[i + 3];
            }
        }
    } else {
        std::memcpy(out, raw, static_cast<size_t>(bufBytes));
    }

    vkUnmapMemory(VKDEV, stageMem);
    vkFreeMemory(VKDEV, stageMem, nullptr);
    vkDestroyBuffer(VKDEV, staging, nullptr);
    return true;
}

// --- pipeline cache ----------------------------------------------------------

PipelineHandle Renderer::getOrCreatePipeline(const PipelineDesc& desc) {
    const uint64_t hash = hashPipelineDesc(desc);
    const auto it = mPipelineCache.find(hash);
    if (it != mPipelineCache.end() && it->second.desc == desc) {
        return reinterpret_cast<PipelineHandle>(it->second.pipeline);
    }

    // --- per-pipeline layout: push constants + optional set 0 ----------------
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = kPushConstantSize;

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (desc.textureCount > 0) {
        VkDescriptorSetLayoutBinding texBinding{};
        texBinding.binding = 0;
        texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texBinding.descriptorCount = desc.textureCount;
        texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsci.bindingCount = 1;
        dsci.pBindings = &texBinding;
        vkCreateDescriptorSetLayout(VKDEV, &dsci, nullptr, &descriptorSetLayout);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = reinterpret_cast<VkDescriptorPool>(mDescriptorPool);
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &descriptorSetLayout;
        vkAllocateDescriptorSets(VKDEV, &dsai, &descriptorSet);
    }

    // M5.4 (TEV): set 1 = per-draw fragment UBO (dynamic). One descriptor per
    // fragmentUbo pipeline, bound to the whole arena; uploadFragmentUbo picks
    // a region via the dynamic offset.
    VkDescriptorSetLayout uboSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet uboSet = VK_NULL_HANDLE;
    if (desc.fragmentUbo) {
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding = 0;
        uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        uboBinding.descriptorCount = 1;
        uboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsci.bindingCount = 1;
        dsci.pBindings = &uboBinding;
        vkCreateDescriptorSetLayout(VKDEV, &dsci, nullptr, &uboSetLayout);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = reinterpret_cast<VkDescriptorPool>(mDescriptorPool);
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &uboSetLayout;
        if (vkAllocateDescriptorSets(VKDEV, &dsai, &uboSet) == VK_SUCCESS) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = reinterpret_cast<VkBuffer>(mUboBuffer);
            bufInfo.offset = 0;
            bufInfo.range = mUboStride;
            VkWriteDescriptorSet uboWrite{};
            uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            uboWrite.dstSet = uboSet;
            uboWrite.dstBinding = 0;
            uboWrite.descriptorCount = 1;
            uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            uboWrite.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(VKDEV, 1, &uboWrite, 0, nullptr);
        }
    }

    VkDescriptorSetLayout layouts[2]{descriptorSetLayout, uboSetLayout};
    uint32_t layoutCount = 0;
    if (descriptorSetLayout) layouts[layoutCount++] = descriptorSetLayout;
    if (uboSetLayout) layouts[layoutCount++] = uboSetLayout;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    plci.setLayoutCount = layoutCount;
    plci.pSetLayouts = layouts;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(VKDEV, &plci, nullptr, &layout);

    // --- shader modules -----------------------------------------------------
    VkShaderModule vertModule = VK_NULL_HANDLE, fragModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vsci{};
    vsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsci.codeSize = desc.vertSpvSize;
    vsci.pCode = desc.vertSpv;
    if (desc.vertSpv && desc.vertSpvSize > 0) {
        vkCreateShaderModule(VKDEV, &vsci, nullptr, &vertModule);
    }
    VkShaderModuleCreateInfo fsci{};
    fsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsci.codeSize = desc.fragSpvSize;
    fsci.pCode = desc.fragSpv;
    if (desc.fragSpv && desc.fragSpvSize > 0) {
        vkCreateShaderModule(VKDEV, &fsci, nullptr, &fragModule);
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t stageCount = 0;
    if (vertModule) {
        stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[stageCount].module = vertModule;
        stages[stageCount].pName = "main";
        ++stageCount;
    }
    if (fragModule) {
        stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[stageCount].module = fragModule;
        stages[stageCount].pName = "main";
        ++stageCount;
    }

    // --- vertex input -------------------------------------------------------
    std::vector<VkVertexInputAttributeDescription> attribs;
    attribs.reserve(desc.vertexLayout.attribs.size());
    for (const auto& a : desc.vertexLayout.attribs) {
        attribs.push_back({a.location, 0, vertexFormatToVk(a.format), a.offset});
    }
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.vertexLayout.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vi.pVertexAttributeDescriptions = attribs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = desc.topology == PrimitiveTopology::TriangleList
                      ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                      : desc.topology == PrimitiveTopology::TriangleStrip
                            ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                            : desc.topology == PrimitiveTopology::TriangleFan
                                  ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                                  : desc.topology == PrimitiveTopology::LineList
                                        ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                        : desc.topology == PrimitiveTopology::LineStrip
                                              ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                                              : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cullModeToVk(desc.cullMode);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0;
    if (desc.colorWrite) {
        blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT;
    }
    if (desc.alphaWrite) {
        blend.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    cb.blendConstants[3] = desc.dstAlphaValue; // CONSTANT_ALPHA factor value
    if (desc.logicOpEnable) {
        // Logic op replaces blending (Vulkan: blending is disabled when the
        // logic op is enabled). GXLogicOp values are 1:1 with VkLogicOp.
        cb.logicOpEnable = VK_TRUE;
        cb.logicOp = static_cast<VkLogicOp>(desc.logicOp);
    } else if (desc.blendEnable) {
        const bool dstAlphaConst = desc.dstAlphaEnable;
        blend.blendEnable = VK_TRUE;
        // GX applies the same factor to all four channels (no separate alpha
        // factors), so the alpha slot mirrors the color slot.
        blend.srcColorBlendFactor = blendFactorToVk(desc.srcBlendFactor, dstAlphaConst);
        blend.dstColorBlendFactor = blendFactorToVk(desc.dstBlendFactor, dstAlphaConst);
        blend.colorBlendOp = blendOpToVk(desc.blendOp);
        blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
        blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
        blend.alphaBlendOp = blend.colorBlendOp;
    }

    // A pipeline that renders into a pass without a depth attachment cannot
    // depth test/write — force the state off so it matches the pass.
    const bool hasDepthAtt = desc.depthFormat != TextureFormat::Undefined;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (desc.depthTest && hasDepthAtt) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (desc.depthWrite && hasDepthAtt) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = compareOpToVk(desc.depthCompare);

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamicStates;

    // Dynamic rendering: declare the color (+ optional depth) attachment
    // formats; they must match the current pass.
    const VkFormat colorFormat = colorFormatToVk(desc.colorFormat);
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    if (hasDepthAtt) {
        renderingInfo.depthAttachmentFormat = textureFormatToVk(desc.depthFormat);
    }

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &renderingInfo;
    pci.stageCount = stageCount;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.pDynamicState = &dyn;
    pci.layout = layout;
    pci.renderPass = VK_NULL_HANDLE; // dynamic rendering
    pci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(VKDEV, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    if (vertModule) vkDestroyShaderModule(VKDEV, vertModule, nullptr);
    if (fragModule) vkDestroyShaderModule(VKDEV, fragModule, nullptr);
    if (result != VK_SUCCESS) {
        PL_LOG_ERROR("renderer", "vkCreateGraphicsPipelines failed (%d)", static_cast<int>(result));
        if (descriptorSetLayout) vkDestroyDescriptorSetLayout(VKDEV, descriptorSetLayout, nullptr);
        if (uboSetLayout) vkDestroyDescriptorSetLayout(VKDEV, uboSetLayout, nullptr);
        vkDestroyPipelineLayout(VKDEV, layout, nullptr);
        return nullptr;
    }

    PipelineCacheEntry entry{desc, reinterpret_cast<void*>(pipeline), reinterpret_cast<void*>(layout),
                             reinterpret_cast<void*>(descriptorSetLayout),
                             reinterpret_cast<void*>(descriptorSet),
                             reinterpret_cast<void*>(uboSetLayout),
                             reinterpret_cast<void*>(uboSet)};
    mPipelineCache[hash] = entry;
    mPipelineByHandle[reinterpret_cast<void*>(pipeline)] = &mPipelineCache[hash];
    return reinterpret_cast<PipelineHandle>(pipeline);
}

void Renderer::bindPipeline(PipelineHandle pipeline) {
    if (!pipeline) {
        return;
    }
    const auto it = mPipelineByHandle.find(pipeline);
    if (it == mPipelineByHandle.end()) {
        PL_LOG_ERROR("renderer", "bindPipeline: unknown pipeline handle");
        return;
    }
    mBoundEntry = it->second;
    vkCmdBindPipeline(reinterpret_cast<VkCommandBuffer>(mCmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      reinterpret_cast<VkPipeline>(pipeline));
}

void Renderer::setUniforms(const void* data, uint32_t size) {
    if (size > kPushConstantSize) {
        PL_LOG_ERROR("renderer", "setUniforms: %u bytes exceeds push constant limit %u", size,
                     kPushConstantSize);
        return;
    }
    if (!mBoundEntry || !mBoundEntry->layout) {
        PL_LOG_ERROR("renderer", "setUniforms: no pipeline bound");
        return;
    }
    vkCmdPushConstants(reinterpret_cast<VkCommandBuffer>(mCmd),
                       reinterpret_cast<VkPipelineLayout>(mBoundEntry->layout),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, size, data);
}

// --- draw -------------------------------------------------------------------

void Renderer::draw(uint32_t vertexCount, uint32_t firstVertex) {
    vkCmdDraw(reinterpret_cast<VkCommandBuffer>(mCmd), vertexCount, 1, firstVertex, 0);
}

void Renderer::drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) {
    vkCmdDrawIndexed(reinterpret_cast<VkCommandBuffer>(mCmd), indexCount, 1, firstIndex,
                     vertexOffset, 0);
}

} // namespace Platform
