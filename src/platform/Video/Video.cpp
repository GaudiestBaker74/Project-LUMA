// =============================================================================
// Platform::Video — windowed Vulkan backend (M3). See Video.h for scope.
//
// Implementation notes:
//   * All Vulkan entry points come from volk (no link-time dependency on
//     vulkan-1; the loader is loaded at runtime).
//   * SDL3 provides the window surface (SDL_Vulkan_CreateSurface) — this is
//     what keeps the file platform-agnostic (X11/Wayland on Linux, Win32 on
//     Windows are SDL's problem, not ours).
//   * Frame sync is deliberately simple for M3: one frame in flight, fence +
//     two semaphores. M4's renderer replaces this with proper frame pacing.
//   * Handles are kept as void* in Video.h so the public header does not
//     depend on volk; the real types live here.
// =============================================================================

#include "platform/Video/Video.h"
#include "platform/Video/VideoCommon.h"
#include "platform/Video/vk_demo_shaders.h"

#include "platform/Log/Log.h"
#include "platform/Timing/Timing.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace Platform {

using namespace Platform::VideoDetail; // vkCheck/stringify/pickPhysicalDevice

// --- handle accessors --------------------------------------------------------
#define VKINST   reinterpret_cast<VkInstance>(mInstance)
#define VKDEV    reinterpret_cast<VkDevice>(mDevice)
#define VKPHYS   reinterpret_cast<VkPhysicalDevice>(mPhysicalDevice)
#define VKSURF   reinterpret_cast<VkSurfaceKHR>(mSurface)
#define VKSWAP   reinterpret_cast<VkSwapchainKHR>(mSwapchain)
#define VKGQ     reinterpret_cast<VkQueue>(mGraphicsQueue)
#define VKPQ     reinterpret_cast<VkQueue>(mPresentQueue)

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

struct DemoVertex {
    float x, y;
    float r, g, b;
};

const DemoVertex kTriangleVertices[] = {
    //      pos                 color
    {  0.0f, -0.6f,   1.0f, 0.2f, 0.2f },
    {  0.6f,  0.6f,   0.2f, 1.0f, 0.2f },
    { -0.6f,  0.6f,   0.2f, 0.2f, 1.0f },
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    // Route Vulkan validation messages into Platform::Log.
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        PL_LOG_ERROR("video.vk", "%s", data->pMessage ? data->pMessage : "?");
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        PL_LOG_WARN("video.vk", "%s", data->pMessage ? data->pMessage : "?");
    } else {
        PL_LOG_DEBUG("video.vk", "%s", data->pMessage ? data->pMessage : "?");
    }
    return VK_FALSE;
}

VkBool32 hasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) {
            return VK_TRUE;
        }
    }
    return VK_FALSE;
}

} // namespace

// =============================================================================

Video& Video::instance() {
    static Video sInstance;
    return sInstance;
}

bool Video::init(SDL_Window* window, const VideoConfig& config) {
    Video& v = instance();
    if (v.isInitialized()) {
        return true;
    }
    v.mValidation = config.enableValidation;
    v.mVsync = config.vsync;

    if (volkInitialize() != VK_SUCCESS) {
        PL_LOG_FATAL("video", "volkInitialize failed — Vulkan loader not available");
        return false;
    }
    v.mWindow = window;
    if (!v.createInstance(window)) {
        return false;
    }
    if (!v.createDeviceAndSwapchain(window)) {
        v.shutdown();
        return false;
    }
    return true;
}

bool Video::createInstance(SDL_Window* window) {
    (void)window;

    // SDL loads the Vulkan loader itself (shared with volk's).
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        PL_LOG_FATAL("video", "SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "galaxy-pc";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "galaxy-pc";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // Instance extensions: everything SDL needs for the surface + our debug.
    std::vector<const char*> extensions;
    uint32_t sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) {
        PL_LOG_FATAL("video", "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }
    extensions.assign(sdlExts, sdlExts + sdlExtCount);
    if (mValidation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    std::vector<const char*> layers;
    if (mValidation && hasLayer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        PL_LOG_INFO("video", "validation layer enabled");
    } else if (mValidation) {
        PL_LOG_WARN("video", "VK_LAYER_KHRONOS_validation requested but not installed");
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    VkResult result = vkCreateInstance(&ci, nullptr, reinterpret_cast<VkInstance*>(&mInstance));
    if (result != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreateInstance failed: %s", stringify(result));
        return false;
    }
    volkLoadInstance(VKINST);

    if (mValidation) {
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
                                       reinterpret_cast<VkDebugUtilsMessengerEXT*>(&mDebugMessenger));
    }

    // Window surface (SDL abstracts X11/Wayland/Win32).
    if (!SDL_Vulkan_CreateSurface(window, VKINST, nullptr, reinterpret_cast<VkSurfaceKHR*>(&mSurface))) {
        PL_LOG_FATAL("video", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool Video::createDeviceAndSwapchain(SDL_Window* window) {
    mPhysicalDevice = pickPhysicalDevice(VKINST, VKSURF, &mGraphicsFamily, &mPresentFamily);

    // Dynamic rendering: core in Vulkan 1.3 (we request 1.3); also enable the
    // extension for drivers that expose it separately (belt and suspenders).
    VkPhysicalDeviceDynamicRenderingFeatures dynRendering{};
    dynRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynRendering.dynamicRendering = VK_TRUE;

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    // Same queue for graphics + present in the common case; if different,
    // create both.
    uint32_t uniqueQueues[2] = {mGraphicsFamily, mPresentFamily};
    uint32_t queueCount = (mGraphicsFamily == mPresentFamily) ? 1 : 2;
    for (uint32_t i = 0; i < queueCount; ++i) {
        VkDeviceQueueCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = uniqueQueues[i];
        q.queueCount = 1;
        q.pQueuePriorities = &queuePriority;
        queueInfos.push_back(q);
    }

    // Device extensions: swapchain (+ dynamic rendering if the driver wants it
    // declared even on 1.3 — harmless to request when supported).
    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    dci.pQueueCreateInfos = queueInfos.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    dci.ppEnabledExtensionNames = deviceExtensions.data();
    dci.pNext = &dynRendering;

    VkResult result = vkCreateDevice(VKPHYS, &dci, nullptr, reinterpret_cast<VkDevice*>(&mDevice));
    if (result != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreateDevice failed: %s", stringify(result));
        return false;
    }
    volkLoadDevice(VKDEV);

    vkGetDeviceQueue(VKDEV, mGraphicsFamily, 0, reinterpret_cast<VkQueue*>(&mGraphicsQueue));
    vkGetDeviceQueue(VKDEV, mPresentFamily, 0, reinterpret_cast<VkQueue*>(&mPresentQueue));

    // Frame-in-flight fence (pre-signaled so the first beginFrame passes).
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(VKDEV, &fenceInfo, nullptr, reinterpret_cast<VkFence*>(&mFence));

    // Acquire semaphore (binary, created once, reused every frame). The
    // render-finished semaphores are per swapchain image — created in
    // recreateSwapchain() and destroyed in destroySwapchain().
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(VKDEV, &semInfo, nullptr, reinterpret_cast<VkSemaphore*>(&mImageAvailable));

    // Swapchain first: the demo pipeline bakes the swapchain format into
    // VkPipelineRenderingCreateInfo, so it must be created after the format
    // is known (the format is stable across swapchain recreates).
    recreateSwapchain(window);
    if (mSwapchain == VK_NULL_HANDLE) {
        return false;
    }
    return createDemoPipeline();
}

void Video::destroySwapchain() {
    if (!isInitialized()) {
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

void Video::recreateSwapchain(SDL_Window* window) {
    // Query the current window size; 0x0 means minimized — keep old swapchain.
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w == 0 || h == 0) {
        PL_LOG_DEBUG("video", "window minimized, skipping swapchain recreate");
        return;
    }

    destroySwapchain();

    // Choose format.
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

    // Present mode: FIFO = vsync; FIFO_RELAXED if vsync disabled is not the
    // same as tearing — for non-vsync we prefer IMMEDIATE.
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
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
        PL_LOG_FATAL("video", "vkCreateSwapchainKHR failed");
        return;
    }
    vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, nullptr);

    // Per-image render-finished semaphores (see Video.h member comment).
    mRenderFinished.resize(mImageCount);
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < mImageCount; ++i) {
        vkCreateSemaphore(VKDEV, &semInfo, nullptr,
                          reinterpret_cast<VkSemaphore*>(&mRenderFinished[i]));
    }

    mExtentW = static_cast<uint32_t>(w);
    mExtentH = static_cast<uint32_t>(h);

    PL_LOG_INFO("video", "swapchain: %ux%u, %u images, format %d, present %d",
                mExtentW, mExtentH, mImageCount, static_cast<int>(surfaceFormat.format),
                static_cast<int>(presentMode));
}

bool Video::createDemoPipeline() {
    // --- vertex buffer ------------------------------------------------------
    const VkDeviceSize bufferSize = sizeof(kTriangleVertices);
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufferSize;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(VKDEV, &bci, nullptr, reinterpret_cast<VkBuffer*>(&mVertexBuffer)) != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreateBuffer (vertex) failed");
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(VKDEV, reinterpret_cast<VkBuffer>(mVertexBuffer), &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(VKPHYS, &memProps);
    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX) {
        PL_LOG_FATAL("video", "no host-visible+coherent memory type");
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(VKDEV, &mai, nullptr, reinterpret_cast<VkDeviceMemory*>(&mVertexMemory)) != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkAllocateMemory (vertex) failed");
        return false;
    }
    vkBindBufferMemory(VKDEV, reinterpret_cast<VkBuffer>(mVertexBuffer),
                       reinterpret_cast<VkDeviceMemory>(mVertexMemory), 0);
    void* mapped = nullptr;
    vkMapMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(mVertexMemory), 0, bufferSize, 0, &mapped);
    std::memcpy(mapped, kTriangleVertices, bufferSize);
    vkUnmapMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(mVertexMemory));

    // --- pipeline layout (no descriptor sets needed for the demo) ------------
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(VKDEV, &plci, nullptr,
                               reinterpret_cast<VkPipelineLayout*>(&mPipelineLayout)) != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreatePipelineLayout failed");
        return false;
    }

    // --- shaders (embedded SPIR-V) -------------------------------------------
    VkShaderModule vertModule = VK_NULL_HANDLE, fragModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vci.codeSize = sizeof(kTriangleVertSpv);
    vci.pCode = kTriangleVertSpv;
    if (vkCreateShaderModule(VKDEV, &vci, nullptr, &vertModule) != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreateShaderModule (vert) failed");
        return false;
    }
    VkShaderModuleCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fci.codeSize = sizeof(kTriangleFragSpv);
    fci.pCode = kTriangleFragSpv;
    if (vkCreateShaderModule(VKDEV, &fci, nullptr, &fragModule) != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreateShaderModule (frag) failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: pos(2f) + color(3f).
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(DemoVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribs[2]{};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[0].offset = offsetof(DemoVertex, x);
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[1].offset = offsetof(DemoVertex, r);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attribs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    // dynamic viewport/scissor set per frame
    vp.pViewports = nullptr;
    vp.pScissors = nullptr;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    // Dynamic state: viewport + scissor.
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamicStates;

    // Dynamic rendering: declare the color attachment format the pipeline
    // writes to (required when renderPass is VK_NULL_HANDLE).
    const VkFormat colorFormat = static_cast<VkFormat>(mSwapchainFormat);
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &renderingInfo;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = reinterpret_cast<VkPipelineLayout>(mPipelineLayout);
    pci.renderPass = VK_NULL_HANDLE; // dynamic rendering
    pci.subpass = 0;

    if (vkCreateGraphicsPipelines(VKDEV, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  reinterpret_cast<VkPipeline*>(&mPipeline)) != VK_SUCCESS) {
        PL_LOG_FATAL("video", "vkCreateGraphicsPipelines failed");
        vkDestroyShaderModule(VKDEV, fragModule, nullptr);
        vkDestroyShaderModule(VKDEV, vertModule, nullptr);
        return false;
    }
    vkDestroyShaderModule(VKDEV, fragModule, nullptr);
    vkDestroyShaderModule(VKDEV, vertModule, nullptr);
    return true;
}

void Video::destroyDemoPipeline() {
    if (!isInitialized()) {
        return;
    }
    if (mPipeline) {
        vkDestroyPipeline(VKDEV, reinterpret_cast<VkPipeline>(mPipeline), nullptr);
        mPipeline = nullptr;
    }
    if (mPipelineLayout) {
        vkDestroyPipelineLayout(VKDEV, reinterpret_cast<VkPipelineLayout>(mPipelineLayout), nullptr);
        mPipelineLayout = nullptr;
    }
    if (mVertexBuffer) {
        vkDestroyBuffer(VKDEV, reinterpret_cast<VkBuffer>(mVertexBuffer), nullptr);
        mVertexBuffer = nullptr;
    }
    if (mVertexMemory) {
        vkFreeMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(mVertexMemory), nullptr);
        mVertexMemory = nullptr;
    }
}

void Video::shutdown() {
    Video& v = instance();
    if (!v.isInitialized()) {
        return;
    }
    vkDeviceWaitIdle(reinterpret_cast<VkDevice>(v.mDevice));
    v.destroyDemoPipeline();
    v.destroySwapchain();
    if (v.mFence) {
        vkDestroyFence(reinterpret_cast<VkDevice>(v.mDevice), reinterpret_cast<VkFence>(v.mFence), nullptr);
        v.mFence = nullptr;
    }
    if (v.mImageAvailable) {
        vkDestroySemaphore(reinterpret_cast<VkDevice>(v.mDevice),
                           reinterpret_cast<VkSemaphore>(v.mImageAvailable), nullptr);
        v.mImageAvailable = nullptr;
    }
    vkDestroyDevice(reinterpret_cast<VkDevice>(v.mDevice), nullptr);
    v.mDevice = nullptr;
    if (v.mDebugMessenger) {
        vkDestroyDebugUtilsMessengerEXT(reinterpret_cast<VkInstance>(v.mInstance),
                                        reinterpret_cast<VkDebugUtilsMessengerEXT>(v.mDebugMessenger),
                                        nullptr);
        v.mDebugMessenger = nullptr;
    }
    if (v.mSurface) {
        vkDestroySurfaceKHR(reinterpret_cast<VkInstance>(v.mInstance),
                            reinterpret_cast<VkSurfaceKHR>(v.mSurface), nullptr);
        v.mSurface = nullptr;
    }
    vkDestroyInstance(reinterpret_cast<VkInstance>(v.mInstance), nullptr);
    v.mInstance = nullptr;
    SDL_Vulkan_UnloadLibrary();
}

bool Video::beginFrame() {
    if (!isInitialized() || mSwapchain == VK_NULL_HANDLE) {
        return false;
    }
    // One frame in flight: wait for the previous frame's fence.
    VkResult wait = vkWaitForFences(VKDEV, 1, reinterpret_cast<VkFence*>(&mFence), VK_TRUE, UINT64_MAX);
    if (wait != VK_SUCCESS) {
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
    return true;
}

void Video::renderDemo(float /*dtSeconds*/) {
    if (!isInitialized() || mSwapchain == VK_NULL_HANDLE) {
        return;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    // Allocate a one-shot command buffer per frame (simple for M3).
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(VKDEV, &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(VKDEV, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Acquired swapchain image + view for this frame.
    std::vector<VkImage> images(mImageCount);
    vkGetSwapchainImagesKHR(VKDEV, VKSWAP, &mImageCount, images.data());
    const VkImage swapImage = images[mImageIndex];

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = swapImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = static_cast<VkFormat>(mSwapchainFormat);
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView imageView = VK_NULL_HANDLE;
    vkCreateImageView(VKDEV, &vci, nullptr, &imageView);

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

    // --- dynamic rendering --------------------------------------------------
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = imageView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = {mClearR, mClearG, mClearB, mClearA};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {0, 0, mExtentW, mExtentH};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    // GX viewport (compat/gx) or default full-screen.
    VkViewport viewport{};
    if (mViewportSet) {
        viewport.x = mViewportX;
        viewport.y = mViewportY;
        viewport.width = mViewportW;
        viewport.height = mViewportH;
    } else {
        viewport.width = static_cast<float>(mExtentW);
        viewport.height = static_cast<float>(mExtentH);
    }
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {mExtentW, mExtentH}};

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Rotate the demo triangle by the current angle (fixed-timestep driven).
    if (mDemoAngle != 0.0f) {
        const float c = std::cos(mDemoAngle);
        const float s = std::sin(mDemoAngle);
        DemoVertex rotated[3];
        for (int i = 0; i < 3; ++i) {
            rotated[i] = kTriangleVertices[i];
            const float rx = kTriangleVertices[i].x * c - kTriangleVertices[i].y * s;
            const float ry = kTriangleVertices[i].x * s + kTriangleVertices[i].y * c;
            rotated[i].x = rx;
            rotated[i].y = ry;
        }
        void* mapped = nullptr;
        vkMapMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(mVertexMemory), 0, sizeof(rotated), 0, &mapped);
        std::memcpy(mapped, rotated, sizeof(rotated));
        vkUnmapMemory(VKDEV, reinterpret_cast<VkDeviceMemory>(mVertexMemory));
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reinterpret_cast<VkPipeline>(mPipeline));
    VkBuffer vertexBuffer = reinterpret_cast<VkBuffer>(mVertexBuffer);
    VkDeviceSize offsets = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offsets);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR (required before present).
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

    vkEndCommandBuffer(cmd);

    // --- submit + present ---------------------------------------------------
    VkSemaphore imageAvailable = reinterpret_cast<VkSemaphore>(mImageAvailable);
    // Semaphore owned by this swapchain image (never reused while the
    // previous present of that image is still in flight).
    VkSemaphore renderFinished = reinterpret_cast<VkSemaphore>(mRenderFinished[mImageIndex]);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    vkQueueSubmit(VKGQ, 1, &submit, reinterpret_cast<VkFence>(mFence));

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

    // One frame in flight: wait for the submit to finish before destroying
    // the transient pool (command buffer must not be pending) and the image
    // view (must not be in use).
    vkWaitForFences(VKDEV, 1, reinterpret_cast<VkFence*>(&mFence), VK_TRUE, UINT64_MAX);
    vkDestroyImageView(VKDEV, imageView, nullptr);
    vkDestroyCommandPool(VKDEV, pool, nullptr);
}

bool Video::endFrame() {
    // The demo submits inside renderDemo; endFrame handles swapchain
    // invalidation. Present errors are caught there; here we just report.
    if (mSwapchainOutOfDate) {
        mSwapchainOutOfDate = false;
        return false; // caller recreates via onResize
    }
    return true;
}

void Video::waitIdle() {
    if (isInitialized()) {
        vkDeviceWaitIdle(VKDEV);
    }
}

void Video::setClearColor(float r, float g, float b, float a) {
    mClearR = r;
    mClearG = g;
    mClearB = b;
    mClearA = a;
}

void Video::setDemoAngle(float radians) {
    mDemoAngle = radians;
}

void Video::setViewport(float x, float y, float w, float h) {
    mViewportX = x;
    mViewportY = y;
    mViewportW = w;
    mViewportH = h;
    mViewportSet = true;
}

void Video::onResize(int width, int height) {
    if (!isInitialized() || mSwapchain == VK_NULL_HANDLE) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    if (mWindow) {
        recreateSwapchain(reinterpret_cast<SDL_Window*>(mWindow));
    }
}

} // namespace Platform
