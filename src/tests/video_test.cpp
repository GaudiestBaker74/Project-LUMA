// =============================================================================
// Headless Vulkan smoke test (M3).
//
// Creates its own Vulkan instance/device (no window, no surface), renders the
// demo triangle into an offscreen color image with dynamic rendering, copies
// it back to host memory and verifies:
//   * the clear color is present (e.g. dark blue background),
//   * the triangle actually drew (a non-background pixel in the middle).
//
// This is what makes the Vulkan stack testable in CI without a display:
//   - Linux CI: works out of the box with lavapipe (mesa-vulkan-drivers).
//   - Any machine without a Vulkan ICD: the test SKIPS (no failure).
// =============================================================================

#include "tests/test_runner.h"

#include "platform/Renderer/Renderer.h"
#include "platform/Renderer/RendererCommon.h"
#include "platform/Renderer/vk_demo_shaders.h"

#include "compat/gx/GXCompat.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Platform::RenderDetail;

namespace {

struct TestContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;

    static constexpr uint32_t kWidth = 256;
    static constexpr uint32_t kHeight = 256;
};

bool setupInstance(TestContext& t) {
    if (volkInitialize() != VK_SUCCESS) {
        return false; // no Vulkan loader -> skip
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "galaxy-pc-test";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, nullptr, &t.instance) != VK_SUCCESS) {
        return false;
    }
    volkLoadInstance(t.instance);

    t.physical = pickPhysicalDevice(t.instance, VK_NULL_HANDLE, &t.queueFamily, nullptr);

    VkPhysicalDeviceDynamicRenderingFeatures dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dyn.dynamicRendering = VK_TRUE;

    const std::vector<const char*> extensions = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = t.queueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    dci.pNext = &dyn;
    if (vkCreateDevice(t.physical, &dci, nullptr, &t.device) != VK_SUCCESS) {
        return false;
    }
    volkLoadDevice(t.device);
    vkGetDeviceQueue(t.device, t.queueFamily, 0, &t.queue);
    return true;
}

uint32_t findMemoryType(TestContext& t, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(t.physical, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool setupOffscreen(TestContext& t) {
    // Color image (offscreen target).
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {TestContext::kWidth, TestContext::kHeight, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(t.device, &ici, nullptr, &t.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(t.device, t.image, &memReq);
    const uint32_t mt = findMemoryType(t, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(t.device, &mai, nullptr, &t.imageMemory) != VK_SUCCESS) {
        return false;
    }
    vkBindImageMemory(t.device, t.image, t.imageMemory, 0);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(t.device, &vci, nullptr, &t.imageView) != VK_SUCCESS) {
        return false;
    }

    // Readback buffer (host-visible).
    const VkDeviceSize readbackSize = TestContext::kWidth * TestContext::kHeight * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = readbackSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(t.device, &bci, nullptr, &t.readbackBuffer) != VK_SUCCESS) {
        return false;
    }
    vkGetBufferMemoryRequirements(t.device, t.readbackBuffer, &memReq);
    const uint32_t mt2 = findMemoryType(t, memReq.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt2 == UINT32_MAX) {
        return false;
    }
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = mt2;
    if (vkAllocateMemory(t.device, &mai, nullptr, &t.readbackMemory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(t.device, t.readbackBuffer, t.readbackMemory, 0);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    if (vkCreateCommandPool(t.device, &pci, nullptr, &t.pool) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool setupPipeline(TestContext& t) {
    // Vertex buffer.
    const float vertices[] = {
        //  x      y      r    g    b
        0.0f, -0.5f, 1.0f, 0.2f, 0.2f,
        0.5f,  0.5f, 0.2f, 1.0f, 0.2f,
        -0.5f, 0.5f, 0.2f, 0.2f, 1.0f,
    };
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(vertices);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(t.device, &bci, nullptr, &t.vertexBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(t.device, t.vertexBuffer, &memReq);
    const uint32_t mt = findMemoryType(t, memReq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(t.device, &mai, nullptr, &t.vertexMemory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(t.device, t.vertexBuffer, t.vertexMemory, 0);
    void* mapped = nullptr;
    vkMapMemory(t.device, t.vertexMemory, 0, sizeof(vertices), 0, &mapped);
    std::memcpy(mapped, vertices, sizeof(vertices));
    vkUnmapMemory(t.device, t.vertexMemory);

    // The demo vertex shader reads a mat3 rotation from push constants
    // (Platform::Renderer::setUniforms); the test pushes an identity rotation.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 48; // mat3, std430
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(t.device, &plci, nullptr, &t.layout) != VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo vsci{};
    vsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsci.codeSize = sizeof(kTriangleVertSpv);
    vsci.pCode = kTriangleVertSpv;
    VkShaderModule vs = VK_NULL_HANDLE;
    vkCreateShaderModule(t.device, &vsci, nullptr, &vs);
    VkShaderModuleCreateInfo fsci{};
    fsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsci.codeSize = sizeof(kTriangleFragSpv);
    fsci.pCode = kTriangleFragSpv;
    VkShaderModule fs = VK_NULL_HANDLE;
    vkCreateShaderModule(t.device, &fsci, nullptr, &fs);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 5 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attribs[2]{};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[0].offset = 0;
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[1].offset = 2 * sizeof(float);

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

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = t.layout;
    if (vkCreateGraphicsPipelines(t.device, VK_NULL_HANDLE, 1, &pci, nullptr, &t.pipeline) != VK_SUCCESS) {
        return false;
    }
    vkDestroyShaderModule(t.device, vs, nullptr);
    vkDestroyShaderModule(t.device, fs, nullptr);
    return true;
}

void runFrame(TestContext& t) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = t.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(t.device, &ai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Transition image to color attachment.
    VkImageMemoryBarrier toAttach{};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = t.image;
    toAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toAttach.srcAccessMask = 0;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttach);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = t.imageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = {0.1f, 0.1f, 0.15f, 1.0f}; // dark blue-ish background

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(TestContext::kWidth),
                  static_cast<float>(TestContext::kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.pipeline);
    VkBuffer vb = t.vertexBuffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

    // Identity rotation (column-major mat3, 48 bytes, std430) so the pushed
    // matrix maps the triangle 1:1 onto the viewport.
    const float identity[12] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0};
    vkCmdPushConstants(cmd, t.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity),
                       identity);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // Transition to transfer source and copy to readback buffer.
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = t.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {TestContext::kWidth, TestContext::kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readbackBuffer, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(t.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(t.queue);

    vkFreeCommandBuffers(t.device, t.pool, 1, &cmd);
}

void teardown(TestContext& t) {
    if (t.device) {
        vkDeviceWaitIdle(t.device);
        if (t.pipeline) vkDestroyPipeline(t.device, t.pipeline, nullptr);
        if (t.layout) vkDestroyPipelineLayout(t.device, t.layout, nullptr);
        if (t.vertexBuffer) vkDestroyBuffer(t.device, t.vertexBuffer, nullptr);
        if (t.vertexMemory) vkFreeMemory(t.device, t.vertexMemory, nullptr);
        if (t.imageView) vkDestroyImageView(t.device, t.imageView, nullptr);
        if (t.image) vkDestroyImage(t.device, t.image, nullptr);
        if (t.imageMemory) vkFreeMemory(t.device, t.imageMemory, nullptr);
        if (t.readbackBuffer) vkDestroyBuffer(t.device, t.readbackBuffer, nullptr);
        if (t.readbackMemory) vkFreeMemory(t.device, t.readbackMemory, nullptr);
        if (t.pool) vkDestroyCommandPool(t.device, t.pool, nullptr);
        vkDestroyDevice(t.device, nullptr);
        t.device = VK_NULL_HANDLE;
    }
    if (t.instance) {
        vkDestroyInstance(t.instance, nullptr);
        t.instance = VK_NULL_HANDLE;
    }
}

TEST_CASE(vulkan_offscreen_triangle) {
    TestContext ctx;
    if (!setupInstance(ctx)) {
        // No Vulkan loader/ICD on this machine — skip rather than fail
        // (CI without a driver, e.g. some Windows runners).
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));
    REQUIRE(setupPipeline(ctx));

    runFrame(ctx);

    // Read back and verify.
    const uint8_t* pixels = nullptr;
    vkMapMemory(ctx.device, ctx.readbackMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels);

    // Background color is (0.1, 0.1, 0.15) -> ~(26, 26, 38) in u8.
    const auto isBackground = [](uint8_t r, uint8_t g, uint8_t b) {
        return r >= 15 && r <= 40 && g >= 15 && g <= 40 && b >= 25 && b <= 55;
    };

    // Corner pixel must be the background (triangle doesn't reach there).
    const uint32_t corner = 0;
    const uint8_t* c = pixels + corner * 4;
    CHECK(isBackground(c[0], c[1], c[2]));

    // Center pixel must be inside the triangle -> NOT background.
    const uint32_t center = (TestContext::kHeight / 2) * TestContext::kWidth + (TestContext::kWidth / 2);
    const uint8_t* m = pixels + center * 4;
    CHECK(!isBackground(m[0], m[1], m[2]));
    CHECK(m[3] == 255); // alpha

    vkUnmapMemory(ctx.device, ctx.readbackMemory);
    teardown(ctx);
}

// =============================================================================
// M4.2: textured quad rendered offscreen. Verifies the full texture path on a
// real driver: staging upload -> sampled image -> descriptor set -> fragment
// shader sampling -> render target (offscreen color image).
// =============================================================================

namespace {

void uploadTextureData(TestContext& t, VkImage image, const void* data, uint32_t w, uint32_t h) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    vkCreateBuffer(t.device, &bci, nullptr, &staging);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(t.device, staging, &memReq);
    const uint32_t mt = findMemoryType(t, memReq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = mt;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    vkAllocateMemory(t.device, &mai, nullptr, &stagingMem);
    vkBindBufferMemory(t.device, staging, stagingMem, 0);
    void* mapped = nullptr;
    vkMapMemory(t.device, stagingMem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    vkUnmapMemory(t.device, stagingMem);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = t.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(t.device, &ai, &cmd);
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
    region.imageExtent = {w, h, 1};
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
    vkQueueSubmit(t.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(t.queue);
    vkFreeCommandBuffers(t.device, t.pool, 1, &cmd);
    vkDestroyBuffer(t.device, staging, nullptr);
    vkFreeMemory(t.device, stagingMem, nullptr);
}

} // namespace

TEST_CASE(vulkan_offscreen_textured_quad) {
    TestContext ctx;
    if (!setupInstance(ctx)) {
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));

    // --- 4x4 checkerboard (RGBA8): black/white cells ------------------------
    constexpr uint32_t kTex = 4;
    std::vector<uint32_t> texData(kTex * kTex);
    for (uint32_t y = 0; y < kTex; ++y) {
        for (uint32_t x = 0; x < kTex; ++x) {
            texData[y * kTex + x] = ((x + y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }

    // --- texture image + view ------------------------------------------------
    VkImage texture = VK_NULL_HANDLE;
    VkDeviceMemory textureMem = VK_NULL_HANDLE;
    VkImageView textureView = VK_NULL_HANDLE;
    {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {kTex, kTex, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        REQUIRE(vkCreateImage(ctx.device, &ici, nullptr, &texture) == VK_SUCCESS);

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(ctx.device, texture, &memReq);
        const uint32_t mt = findMemoryType(ctx, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        REQUIRE(mt != UINT32_MAX);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = mt;
        REQUIRE(vkAllocateMemory(ctx.device, &mai, nullptr, &textureMem) == VK_SUCCESS);
        vkBindImageMemory(ctx.device, texture, textureMem, 0);

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = texture;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        REQUIRE(vkCreateImageView(ctx.device, &vci, nullptr, &textureView) == VK_SUCCESS);
    }
    uploadTextureData(ctx, texture, texData.data(), kTex, kTex);

    // --- sampler (nearest + clamp: exact texel colors) -----------------------
    VkSampler sampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        REQUIRE(vkCreateSampler(ctx.device, &sci, nullptr, &sampler) == VK_SUCCESS);
    }

    // --- descriptor set (set 0, binding 0) -----------------------------------
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsci.bindingCount = 1;
        dsci.pBindings = &b;
        vkCreateDescriptorSetLayout(ctx.device, &dsci, nullptr, &setLayout);

        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps;
        vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &descPool);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        vkAllocateDescriptorSets(ctx.device, &dsai, &descSet);

        VkDescriptorImageInfo info{};
        info.sampler = sampler;
        info.imageView = textureView;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = descSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    }

    // --- pipeline: tri_tex shaders, pos(2f)+uv(2f) ---------------------------
    struct QuadVertex {
        float x, y, u, v;
    };
    const QuadVertex quad[6] = {
        {-1.0f, -1.0f, 0.0f, 0.0f}, { 1.0f, -1.0f, 1.0f, 0.0f}, { 1.0f, 1.0f, 1.0f, 1.0f},
        {-1.0f, -1.0f, 0.0f, 0.0f}, { 1.0f,  1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 0.0f, 1.0f},
    };
    VkBufferCreateInfo qbci{};
    qbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    qbci.size = sizeof(quad);
    qbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer quadBuf = VK_NULL_HANDLE;
    vkCreateBuffer(ctx.device, &qbci, nullptr, &quadBuf);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(ctx.device, quadBuf, &memReq);
    const uint32_t qmt = findMemoryType(ctx, memReq.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = qmt;
    VkDeviceMemory quadMem = VK_NULL_HANDLE;
    vkAllocateMemory(ctx.device, &mai, nullptr, &quadMem);
    vkBindBufferMemory(ctx.device, quadBuf, quadMem, 0);
    void* qmapped = nullptr;
    vkMapMemory(ctx.device, quadMem, 0, sizeof(quad), 0, &qmapped);
    std::memcpy(qmapped, quad, sizeof(quad));
    vkUnmapMemory(ctx.device, quadMem);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 48;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(ctx.device, &plci, nullptr, &layout);

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vsci{};
    vsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsci.codeSize = sizeof(kTriangleTexVertSpv);
    vsci.pCode = kTriangleTexVertSpv;
    vkCreateShaderModule(ctx.device, &vsci, nullptr, &vs);
    VkShaderModuleCreateInfo fsci{};
    fsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsci.codeSize = sizeof(kTriangleTexFragSpv);
    fsci.pCode = kTriangleTexFragSpv;
    vkCreateShaderModule(ctx.device, &fsci, nullptr, &fs);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(QuadVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attribs[2]{};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[0].offset = 0;
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[1].offset = 8;
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
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
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
    pci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    REQUIRE(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) == VK_SUCCESS);
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);

    // --- render --------------------------------------------------------------
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = ctx.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toAttach{};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = ctx.image;
    toAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toAttach.srcAccessMask = 0;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttach);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = ctx.imageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = {0.1f, 0.1f, 0.15f, 1.0f};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp2{0.0f, 0.0f, static_cast<float>(TestContext::kWidth),
                   static_cast<float>(TestContext::kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descSet, 0, nullptr);
    VkBuffer vb = quadBuf;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    const float identity[12] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity), identity);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = ctx.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {TestContext::kWidth, TestContext::kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, ctx.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx.readbackBuffer, 1, &copyRegion);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.queue);
    vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cmd);

    // --- verify checkerboard (each cell is 64 px; centers sampled) ------------
    const uint8_t* pixels = nullptr;
    vkMapMemory(ctx.device, ctx.readbackMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels);

    const auto at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels + (y * TestContext::kWidth + x) * 4;
    };
    const auto isBlack = [](const uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
    const auto isWhite = [](const uint8_t* p) { return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255; };

    // cell (0,0) black, (1,0) white, (0,1) white, (1,1) black.
    CHECK(isBlack(at(32, 32)));
    CHECK(isWhite(at(96, 32)));
    CHECK(isWhite(at(32, 96)));
    CHECK(isBlack(at(96, 96)));

    // Far corner also black (cell (0,0) area is only quadrant; (3,3) is black).
    CHECK(isBlack(at(224, 224))); // cell (3,3): (3+3)&1=0 -> black

    vkUnmapMemory(ctx.device, ctx.readbackMemory);

    // --- cleanup --------------------------------------------------------------
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    vkDestroyBuffer(ctx.device, quadBuf, nullptr);
    vkFreeMemory(ctx.device, quadMem, nullptr);
    vkDestroyDescriptorPool(ctx.device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, setLayout, nullptr);
    vkDestroySampler(ctx.device, sampler, nullptr);
    vkDestroyImageView(ctx.device, textureView, nullptr);
    vkDestroyImage(ctx.device, texture, nullptr);
    vkFreeMemory(ctx.device, textureMem, nullptr);
    teardown(ctx);
}

} // namespace

// =============================================================================
// M5.1: GX quad rendered offscreen with the GX shaders (kGxVertSpv/kGxFragSpv).
//
// Mirrors exactly what compat/gx::flushDraw submits to Platform::Renderer for
// a GX quad (vertex data serialized in VCD order: pos3 + color0 4 floats,
// 28-byte stride; identity MVP; one shared buffer with per-range draw
// offsets). Verifies the interpolated vertex colors in the four quadrants of
// the offscreen image. The CPU-side capture (state mirror -> FIFO -> VCD
// serialization, direct + indexed) is covered headless in gx_test.cpp; this
// test pins the GPU side (layout offsets + shaders + multi-draw offsets).
// =============================================================================

namespace {

bool setupGxQuadPipeline(TestContext& t) {
    // Quad as two triangles, vertices in VCD order (pos3 f32 + clr0 rgba f32).
    // Colors match the demo quad: BL red, BR green, TR blue, TL yellow.
    const float quad[6 * 7] = {
        // Triangle 1: BL, BR, TR
        -0.6f, -0.6f, 0.0f, 1.0f, 0.2f, 0.2f, 1.0f,
         0.6f, -0.6f, 0.0f, 0.2f, 1.0f, 0.2f, 1.0f,
         0.6f,  0.6f, 0.0f, 0.2f, 0.2f, 1.0f, 1.0f,
        // Triangle 2: BL, TR, TL
        -0.6f, -0.6f, 0.0f, 1.0f, 0.2f, 0.2f, 1.0f,
         0.6f,  0.6f, 0.0f, 0.2f, 0.2f, 1.0f, 1.0f,
        -0.6f,  0.6f, 0.0f, 1.0f, 1.0f, 0.2f, 1.0f,
    };
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(quad);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(t.device, &bci, nullptr, &t.vertexBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(t.device, t.vertexBuffer, &memReq);
    const uint32_t mt = findMemoryType(t, memReq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(t.device, &mai, nullptr, &t.vertexMemory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(t.device, t.vertexBuffer, t.vertexMemory, 0);
    void* mapped = nullptr;
    vkMapMemory(t.device, t.vertexMemory, 0, sizeof(quad), 0, &mapped);
    std::memcpy(mapped, quad, sizeof(quad));
    vkUnmapMemory(t.device, t.vertexMemory);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 64; // mat4 MVP
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(t.device, &plci, nullptr, &t.layout) != VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo vsci{};
    vsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsci.codeSize = sizeof(kGxVertSpv);
    vsci.pCode = kGxVertSpv;
    VkShaderModule vs = VK_NULL_HANDLE;
    vkCreateShaderModule(t.device, &vsci, nullptr, &vs);
    VkShaderModuleCreateInfo fsci{};
    fsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsci.codeSize = sizeof(kGxFragSpv);
    fsci.pCode = kGxFragSpv;
    VkShaderModule fs = VK_NULL_HANDLE;
    vkCreateShaderModule(t.device, &fsci, nullptr, &fs);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 7 * sizeof(float); // VCD: pos3 + clr0 4
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attribs[2]{};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[0].offset = 0;
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribs[1].offset = 3 * sizeof(float);

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

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = t.layout;
    if (vkCreateGraphicsPipelines(t.device, VK_NULL_HANDLE, 1, &pci, nullptr, &t.pipeline) != VK_SUCCESS) {
        return false;
    }
    vkDestroyShaderModule(t.device, vs, nullptr);
    vkDestroyShaderModule(t.device, fs, nullptr);
    return true;
}

void runGxQuadFrame(TestContext& t) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = t.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(t.device, &ai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toAttach{};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = t.image;
    toAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toAttach.srcAccessMask = 0;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttach);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = t.imageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = {0.1f, 0.1f, 0.15f, 1.0f};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(TestContext::kWidth),
                  static_cast<float>(TestContext::kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.pipeline);
    VkBuffer vb = t.vertexBuffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

    // Identity MVP (GX clip-space matrix; the quad is already in NDC).
    const float identity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    vkCmdPushConstants(cmd, t.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity), identity);
    // Two draws over the SAME buffer at different firstVertex offsets — the
    // M5.2 dynamic-buffer pattern (GXCompat appends every primitive to one
    // shared vertex buffer and draws each range with an offset).
    vkCmdDraw(cmd, 3, 1, 0, 0); // triangle 1 (BL, BR, TR)
    vkCmdDraw(cmd, 3, 1, 3, 0); // triangle 2 (BL, TR, TL)
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = t.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {TestContext::kWidth, TestContext::kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readbackBuffer, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(t.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(t.queue);
    vkFreeCommandBuffers(t.device, t.pool, 1, &cmd);
}

} // namespace

TEST_CASE(vulkan_offscreen_gx_quad) {
    TestContext ctx;
    if (!setupInstance(ctx)) {
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));
    REQUIRE(setupGxQuadPipeline(ctx));

    runGxQuadFrame(ctx);

    const uint8_t* pixels = nullptr;
    vkMapMemory(ctx.device, ctx.readbackMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels);

    // Expected interpolated vertex color at one point per quadrant, picked OFF
    // the BL->TR diagonal (points on y==x sit on the shared triangle edge and
    // interpolate ambiguously). Barycentric over the quad's two triangles with
    // vertex colors 1.0/0.2/0.2 style (BL red, BR green, TR blue, TL yellow).
    // Pixel coords use the Vulkan viewport (NDC y=+1 at the BOTTOM):
    //   P1 NDC(-0.15,-0.45) tri1: 0.625 BL + 0.25 BR + 0.125 TR -> (179,102, 77)
    //   P2 NDC( 0.30,-0.30) tri1: 0.25  BL + 0.50 BR + 0.25  TR -> (102,153,102)
    //   P3 NDC(-0.30, 0.30) tri2: 0.25  BL + 0.25 TR + 0.50  TL -> (204,153,102)
    //   P4 NDC( 0.15, 0.45) tri2: 0.125 BL + 0.625 TR + 0.25  TL -> (128,102,179)
    // Tolerances absorb u8 rounding + sample-position rounding.
    const struct { uint32_t x, y; uint8_t r, g, b; } expected[4] = {
        {109,  70, 179, 102,  77},
        {166,  90, 102, 153, 102},
        { 90, 166, 204, 153, 102},
        {147, 186, 128, 102, 179},
    };
    for (const auto& e : expected) {
        const uint8_t* p = pixels + (e.y * TestContext::kWidth + e.x) * 4;
        CHECK(std::abs(int(p[0]) - e.r) <= 25);
        CHECK(std::abs(int(p[1]) - e.g) <= 25);
        CHECK(std::abs(int(p[2]) - e.b) <= 25);
        CHECK(p[3] == 255); // opaque
    }

    vkUnmapMemory(ctx.device, ctx.readbackMemory);
    teardown(ctx);
}

// =============================================================================
// M5.2: dynamic vertex buffer API (Platform::Renderer).
//
// Needs a real (hidden) SDL window + Vulkan surface, so it SKIPs when there
// is no display/driver (CI without X or Vulkan ICD). Validates the contract
// compat/gx relies on: creation, growth (ensureBufferCapacity reallocates and
// keeps the handle valid, preserving content), bounds-checked updates, and
// the multi-primitive append/draw pattern (offset = firstVertex).
// =============================================================================

TEST_CASE(renderer_dynamic_buffer) {
    // NOTE: SDL3's SDL_Init returns bool — true on SUCCESS (the SDL2 return
    // convention was inverted).
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SKIP("SDL_Init failed (no video subsystem)");
        return;
    }
    SDL_Window* window = SDL_CreateWindow("galaxy-pc-test-dynbuf", 128, 128,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!window) {
        SDL_Quit();
        SKIP("SDL_CreateWindow (hidden, Vulkan) failed");
        return;
    }
    Platform::RendererConfig cfg{};
    cfg.appName = "galaxy-pc-tests";
    cfg.enableValidation = false;
    cfg.vsync = false;
    if (!Platform::Renderer::init(window, cfg)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        SKIP("Platform::Renderer init failed (no Vulkan surface/ICD)");
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    REQUIRE(r.isInitialized());

    // Creation + in-bounds update.
    Platform::BufferHandle dyn = r.createDynamicBuffer(64); // clamped to min
    REQUIRE(dyn != nullptr);
    const float a[3] = {1.0f, 2.0f, 3.0f};
    CHECK(r.updateDynamicBuffer(dyn, 0, sizeof(a), a));

    // Out-of-bounds updates are rejected (before and after growth).
    const float junk[4] = {0, 0, 0, 0};
    CHECK(!r.updateDynamicBuffer(dyn, static_cast<uint64_t>(1) << 40, sizeof(junk), junk));
    CHECK(r.ensureBufferCapacity(dyn, 2000));
    CHECK(r.updateDynamicBuffer(dyn, 1500, sizeof(junk), junk)); // now in bounds
    CHECK(!r.updateDynamicBuffer(dyn, static_cast<uint64_t>(1) << 40, sizeof(junk), junk));

    // Growth preserves the handle (same pointer) and content stays valid: the
    // earlier in-bounds write region is still writable.
    CHECK(r.ensureBufferCapacity(dyn, 4096));
    CHECK(r.updateDynamicBuffer(dyn, 0, sizeof(a), a));
    CHECK(r.updateDynamicBuffer(dyn, 4000, sizeof(junk), junk)); // beyond first growth

    r.destroyBuffer(dyn);
    Platform::Renderer::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
}

// =============================================================================
// M5.3: GX textured quad offscreen with the GX tex shaders (kGxTex*Spv).
//
// Mirrors what compat/gx::flushDraw submits for a textured draw: vertex data
// in VCD order (pos3 + clr0 4 + tex0 2 floats, 36-byte stride), mat4 MVP push
// constant, textureCount=1 with the checkerboard at set0/binding0, and
// texel*color modulation (white color -> exact texel colors). Verifies the
// 4x4 checkerboard cell colors pixel by pixel.
// =============================================================================

TEST_CASE(vulkan_offscreen_gx_textured_quad) {
    TestContext ctx;
    if (!setupInstance(ctx)) {
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));

    // 4x4 checkerboard: (x+y)&1 -> white, else black (matches the M4.2 test).
    constexpr uint32_t kTex = 4;
    std::vector<uint32_t> texData(kTex * kTex);
    for (uint32_t y = 0; y < kTex; ++y) {
        for (uint32_t x = 0; x < kTex; ++x) {
            texData[y * kTex + x] = ((x + y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }

    // --- texture + sampler + descriptor set -----------------------------------
    VkImage texture = VK_NULL_HANDLE;
    VkDeviceMemory textureMem = VK_NULL_HANDLE;
    VkImageView textureView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {kTex, kTex, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        REQUIRE(vkCreateImage(ctx.device, &ici, nullptr, &texture) == VK_SUCCESS);
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(ctx.device, texture, &memReq);
        const uint32_t mt = findMemoryType(ctx, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        REQUIRE(mt != UINT32_MAX);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = mt;
        REQUIRE(vkAllocateMemory(ctx.device, &mai, nullptr, &textureMem) == VK_SUCCESS);
        vkBindImageMemory(ctx.device, texture, textureMem, 0);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = texture;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        REQUIRE(vkCreateImageView(ctx.device, &vci, nullptr, &textureView) == VK_SUCCESS);

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        REQUIRE(vkCreateSampler(ctx.device, &sci, nullptr, &sampler) == VK_SUCCESS);

        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsci.bindingCount = 1;
        dsci.pBindings = &b;
        vkCreateDescriptorSetLayout(ctx.device, &dsci, nullptr, &setLayout);
        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps;
        vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &descPool);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        vkAllocateDescriptorSets(ctx.device, &dsai, &descSet);
        VkDescriptorImageInfo info{};
        info.sampler = sampler;
        info.imageView = textureView;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = descSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    }
    uploadTextureData(ctx, texture, texData.data(), kTex, kTex);

    // --- vertex buffer: VCD order pos3 + clr0(4) + tex0(2), 9 floats ---------
    const float quad[6 * 9] = {
        // triangle 1
        -1.0f, -1.0f, 0.0f, 1, 1, 1, 1, 0.0f, 0.0f,
         1.0f, -1.0f, 0.0f, 1, 1, 1, 1, 1.0f, 0.0f,
         1.0f,  1.0f, 0.0f, 1, 1, 1, 1, 1.0f, 1.0f,
        // triangle 2
        -1.0f, -1.0f, 0.0f, 1, 1, 1, 1, 0.0f, 0.0f,
         1.0f,  1.0f, 0.0f, 1, 1, 1, 1, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1, 1, 1, 1, 0.0f, 1.0f,
    };
    VkBufferCreateInfo qbci{};
    qbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    qbci.size = sizeof(quad);
    qbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer quadBuf = VK_NULL_HANDLE;
    vkCreateBuffer(ctx.device, &qbci, nullptr, &quadBuf);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(ctx.device, quadBuf, &memReq);
    const uint32_t qmt = findMemoryType(ctx, memReq.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = qmt;
    VkDeviceMemory quadMem = VK_NULL_HANDLE;
    vkAllocateMemory(ctx.device, &mai, nullptr, &quadMem);
    vkBindBufferMemory(ctx.device, quadBuf, quadMem, 0);
    void* qmapped = nullptr;
    vkMapMemory(ctx.device, quadMem, 0, sizeof(quad), 0, &qmapped);
    std::memcpy(qmapped, quad, sizeof(quad));
    vkUnmapMemory(ctx.device, quadMem);

    // --- pipeline: kGxTex*Spv, mat4 push constant, pos@0 color@1 uv@2 ---------
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 64; // mat4 MVP
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(ctx.device, &plci, nullptr, &layout);

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vsci{};
    vsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsci.codeSize = sizeof(kGxTexVertSpv);
    vsci.pCode = kGxTexVertSpv;
    vkCreateShaderModule(ctx.device, &vsci, nullptr, &vs);
    VkShaderModuleCreateInfo fsci{};
    fsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsci.codeSize = sizeof(kGxTexFragSpv);
    fsci.pCode = kGxTexFragSpv;
    vkCreateShaderModule(ctx.device, &fsci, nullptr, &fs);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 9 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attribs[3]{};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[0].offset = 0;
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribs[1].offset = 3 * sizeof(float);
    attribs[2].location = 2;
    attribs[2].binding = 0;
    attribs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[2].offset = 7 * sizeof(float);
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attribs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
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
    pci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    REQUIRE(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) == VK_SUCCESS);
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);

    // --- render --------------------------------------------------------------
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = ctx.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toAttach{};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = ctx.image;
    toAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toAttach.srcAccessMask = 0;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttach);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = ctx.imageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = {0.1f, 0.1f, 0.15f, 1.0f};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp2{0.0f, 0.0f, static_cast<float>(TestContext::kWidth),
                   static_cast<float>(TestContext::kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descSet, 0, nullptr);
    VkBuffer vb = quadBuf;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    const float identity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity), identity);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = ctx.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {TestContext::kWidth, TestContext::kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, ctx.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx.readbackBuffer, 1, &copyRegion);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.queue);
    vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cmd);

    // --- verify checkerboard texel colors (white vertex color -> exact) ------
    const uint8_t* pixels = nullptr;
    vkMapMemory(ctx.device, ctx.readbackMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels);
    const auto at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels + (y * TestContext::kWidth + x) * 4;
    };
    const auto isBlack = [](const uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
    const auto isWhite = [](const uint8_t* p) { return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255; };

    CHECK(isBlack(at(32, 32)));  // cell (0,0)
    CHECK(isWhite(at(96, 32)));  // cell (1,0)
    CHECK(isWhite(at(32, 96)));  // cell (0,1)
    CHECK(isBlack(at(96, 96)));  // cell (1,1)
    CHECK(isBlack(at(224, 224))); // cell (3,3)
    vkUnmapMemory(ctx.device, ctx.readbackMemory);

    // --- cleanup --------------------------------------------------------------
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    vkDestroyBuffer(ctx.device, quadBuf, nullptr);
    vkFreeMemory(ctx.device, quadMem, nullptr);
    vkDestroyDescriptorPool(ctx.device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, setLayout, nullptr);
    vkDestroySampler(ctx.device, sampler, nullptr);
    vkDestroyImageView(ctx.device, textureView, nullptr);
    vkDestroyImage(ctx.device, texture, nullptr);
    vkFreeMemory(ctx.device, textureMem, nullptr);
    teardown(ctx);
}

// =============================================================================

// =============================================================================
// M5.4/M5.5: GX TEV + pixel-engine (fog/alpha compare) rendered offscreen with
// the TEV shaders (kGxTev*Spv). Exercises the full GPU path end-to-end:
// GX API -> buildTevUbo (std140 UBO) -> 8-texture descriptor set (set0) +
// dynamic UBO (set1) -> fragment shader TEV chain -> pixel-engine output
// stage (alpha compare discard, fog) -> framebuffer. The expected colors are
// computed by the CPU reference (evalTevChain / evalPixelEngine) and by hand.
// =============================================================================

namespace {

// A 1x1 solid RGBA texture (image + view + memory) with a NEAREST/CLAMP
// sampler — the TEV tests only need constant texels.
struct SolidTex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

void createSolidTex(TestContext& ctx, const uint8_t rgba[4], SolidTex& t) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {1, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    REQUIRE(vkCreateImage(ctx.device, &ici, nullptr, &t.image) == VK_SUCCESS);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(ctx.device, t.image, &memReq);
    const uint32_t mt = findMemoryType(ctx, memReq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    REQUIRE(mt != UINT32_MAX);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = mt;
    REQUIRE(vkAllocateMemory(ctx.device, &mai, nullptr, &t.memory) == VK_SUCCESS);
    vkBindImageMemory(ctx.device, t.image, t.memory, 0);
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    REQUIRE(vkCreateImageView(ctx.device, &vci, nullptr, &t.view) == VK_SUCCESS);
    uploadTextureData(ctx, t.image, rgba, 1, 1);
}

// Everything the offscreen TEV tests need after the pipeline is created: the
// two descriptor sets (set0 = 8 combined image samplers, set1 = dynamic UBO),
// the UBO + quad vertex buffers, and the graphics pipeline (kGxTev*Spv with
// the fixed 27-float vertex layout and a mat4 vertex push constant).
struct TevFrameResources {
    VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set1Layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer uboBuf = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    VkBuffer quadBuf = VK_NULL_HANDLE;
    VkDeviceMemory quadMem = VK_NULL_HANDLE;
};

void createTevFrameResources(TestContext& ctx, const void* ubo, size_t uboSize,
                             VkImageView texViews[8], TevFrameResources& r,
                             const float* quadOverride = nullptr) {
    // --- descriptor sets ----------------------------------------------------
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t i = 0; i < 8; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }
        VkDescriptorSetLayoutCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsci.bindingCount = static_cast<uint32_t>(bindings.size());
        dsci.pBindings = bindings.data();
        REQUIRE(vkCreateDescriptorSetLayout(ctx.device, &dsci, nullptr, &r.set0Layout) == VK_SUCCESS);

        VkDescriptorSetLayoutBinding ub{};
        ub.binding = 0;
        ub.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ub.descriptorCount = 1;
        ub.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo usci{};
        usci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        usci.bindingCount = 1;
        usci.pBindings = &ub;
        REQUIRE(vkCreateDescriptorSetLayout(ctx.device, &usci, nullptr, &r.set1Layout) == VK_SUCCESS);

        VkDescriptorPoolSize ps[2]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps[0].descriptorCount = 8;
        ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ps[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 2;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = ps;
        REQUIRE(vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &r.pool) == VK_SUCCESS);

        VkDescriptorSetLayout layouts[2] = {r.set0Layout, r.set1Layout};
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = r.pool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = layouts;
        REQUIRE(vkAllocateDescriptorSets(ctx.device, &dsai, sets) == VK_SUCCESS);
        r.set0 = sets[0];
        r.set1 = sets[1];

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        REQUIRE(vkCreateSampler(ctx.device, &sci, nullptr, &r.sampler) == VK_SUCCESS);

        std::vector<VkDescriptorImageInfo> info(8);
        std::vector<VkWriteDescriptorSet> writes;
        for (uint32_t i = 0; i < 8; ++i) {
            info[i].sampler = r.sampler;
            info[i].imageView = texViews[i];
            info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = r.set0;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &info[i];
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(ctx.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // --- UBO buffer (host-visible + coherent) -------------------------------
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = uboSize;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        REQUIRE(vkCreateBuffer(ctx.device, &bci, nullptr, &r.uboBuf) == VK_SUCCESS);
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(ctx.device, r.uboBuf, &memReq);
        const uint32_t mt = findMemoryType(ctx, memReq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        REQUIRE(mt != UINT32_MAX);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = mt;
        REQUIRE(vkAllocateMemory(ctx.device, &mai, nullptr, &r.uboMem) == VK_SUCCESS);
        vkBindBufferMemory(ctx.device, r.uboBuf, r.uboMem, 0);
        void* mapped = nullptr;
        vkMapMemory(ctx.device, r.uboMem, 0, uboSize, 0, &mapped);
        std::memcpy(mapped, ubo, uboSize);
        vkUnmapMemory(ctx.device, r.uboMem);

        VkDescriptorBufferInfo db{};
        db.buffer = r.uboBuf;
        db.offset = 0;
        db.range = uboSize;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = r.set1;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.pBufferInfo = &db;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    }

    // --- quad vertex buffer (fixed TEV layout: pos3 clr0(4) clr1(4) uv0..7) --
    {
        constexpr int kStride = 27;
        float quad[6 * kStride];
        if (quadOverride) {
            // Caller supplies the full 6-vertex layout (e.g. the lit output of
            // flushDraw's buildVertex — pos, clr0, clr1, uvs).
            std::memcpy(quad, quadOverride, sizeof(quad));
        } else {
            // z = 0.5: Vulkan NDC depth is [0,1] (near=0, far=1), so the identity
            // MVP maps vertex z=0.5 to gl_FragCoord.z = 0.5 and the GX-reversed
            // zCoord to int((1 - 0.5) * 2^24) = 8388608 (half of the 24-bit range).
            const float pos[6][3] = {
                {-0.6f, -0.6f, 0.5f}, {0.6f, -0.6f, 0.5f}, {0.6f, 0.6f, 0.5f},
                {-0.6f, -0.6f, 0.5f}, {0.6f, 0.6f, 0.5f}, {-0.6f, 0.6f, 0.5f},
            };
            for (int v = 0; v < 6; ++v) {
                float* d = quad + v * kStride;
                d[0] = pos[v][0]; d[1] = pos[v][1]; d[2] = pos[v][2];
                d[3] = d[4] = d[5] = d[6] = 1.0f;         // clr0 white
                d[7] = d[8] = d[9] = d[10] = 1.0f;        // clr1 white
                for (int t = 0; t < 8; ++t) { d[11 + 2*t] = 0.5f; d[12 + 2*t] = 0.5f; }
            }
        }
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = sizeof(quad);
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        REQUIRE(vkCreateBuffer(ctx.device, &bci, nullptr, &r.quadBuf) == VK_SUCCESS);
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(ctx.device, r.quadBuf, &memReq);
        const uint32_t mt = findMemoryType(ctx, memReq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        REQUIRE(mt != UINT32_MAX);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = mt;
        REQUIRE(vkAllocateMemory(ctx.device, &mai, nullptr, &r.quadMem) == VK_SUCCESS);
        vkBindBufferMemory(ctx.device, r.quadBuf, r.quadMem, 0);
        void* mapped = nullptr;
        vkMapMemory(ctx.device, r.quadMem, 0, sizeof(quad), 0, &mapped);
        std::memcpy(mapped, quad, sizeof(quad));
        vkUnmapMemory(ctx.device, r.quadMem);
    }

    // --- graphics pipeline ---------------------------------------------------
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 64;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pushRange;
        plci.setLayoutCount = 2;
        VkDescriptorSetLayout pls[2] = {r.set0Layout, r.set1Layout};
        plci.pSetLayouts = pls;
        REQUIRE(vkCreatePipelineLayout(ctx.device, &plci, nullptr, &r.layout) == VK_SUCCESS);

        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo vsci{};
        vsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vsci.codeSize = sizeof(kGxTevVertSpv);
        vsci.pCode = kGxTevVertSpv;
        REQUIRE(vkCreateShaderModule(ctx.device, &vsci, nullptr, &vs) == VK_SUCCESS);
        VkShaderModuleCreateInfo fsci{};
        fsci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fsci.codeSize = sizeof(kGxTevFragSpv);
        fsci.pCode = kGxTevFragSpv;
        REQUIRE(vkCreateShaderModule(ctx.device, &fsci, nullptr, &fs) == VK_SUCCESS);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        constexpr int kStride = 27;
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = kStride * sizeof(float);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attribs[11]{};
        const uint32_t offsets[11] = {0, 12, 28, 44, 52, 60, 68, 76, 84, 92, 100};
        for (int i = 0; i < 11; ++i) {
            attribs[i].location = static_cast<uint32_t>(i);
            attribs[i].binding = 0;
            attribs[i].format = (i == 0) ? VK_FORMAT_R32G32B32_SFLOAT
                                : (i <= 2) ? VK_FORMAT_R32G32B32A32_SFLOAT
                                           : VK_FORMAT_R32G32_SFLOAT;
            attribs[i].offset = offsets[i];
        }
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 11;
        vi.pVertexAttributeDescriptions = attribs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynStates;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dyn;
        pci.layout = r.layout;
        REQUIRE(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                          &r.pipeline) == VK_SUCCESS);
        vkDestroyShaderModule(ctx.device, vs, nullptr);
        vkDestroyShaderModule(ctx.device, fs, nullptr);
    }
}

void destroyTevFrameResources(TestContext& ctx, TevFrameResources& r) {
    vkDestroyPipeline(ctx.device, r.pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, r.layout, nullptr);
    vkDestroyDescriptorPool(ctx.device, r.pool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, r.set0Layout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, r.set1Layout, nullptr);
    vkDestroySampler(ctx.device, r.sampler, nullptr);
    vkDestroyBuffer(ctx.device, r.uboBuf, nullptr);
    vkFreeMemory(ctx.device, r.uboMem, nullptr);
    vkDestroyBuffer(ctx.device, r.quadBuf, nullptr);
    vkFreeMemory(ctx.device, r.quadMem, nullptr);
}

// Records and submits one frame: clear -> draw the full-screen quad with the
// identity MVP (all vertices at z = 0 -> gl_FragCoord.z = 0.5 -> the shader's
// fog zCoord = int((1 - 0.5) * 2^24) = 8388608) -> copy to the readback
// buffer. Returns the mapped readback pixels (caller must unmap).
const uint8_t* runTevQuadFrame(TestContext& ctx, TevFrameResources& r,
                               const void* ubo, size_t uboSize) {
    void* mappedUbo = nullptr;
    vkMapMemory(ctx.device, r.uboMem, 0, uboSize, 0, &mappedUbo);
    std::memcpy(mappedUbo, ubo, uboSize);
    vkUnmapMemory(ctx.device, r.uboMem);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = ctx.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toAttach{};
    toAttach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = ctx.image;
    toAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toAttach.srcAccessMask = 0;
    toAttach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttach);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = ctx.imageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = {0.1f, 0.1f, 0.15f, 1.0f};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(TestContext::kWidth),
                  static_cast<float>(TestContext::kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {TestContext::kWidth, TestContext::kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    const uint32_t dynOffset = 0;
    VkDescriptorSet boundSets[2] = {r.set0, r.set1};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 2,
                            boundSets, 1, &dynOffset);
    VkBuffer vb = r.quadBuf;
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
    const float identity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    vkCmdPushConstants(cmd, r.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity), identity);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = ctx.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {TestContext::kWidth, TestContext::kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, ctx.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx.readbackBuffer, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.queue);
    vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cmd);

    const uint8_t* pixels = nullptr;
    vkMapMemory(ctx.device, ctx.readbackMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels);
    return pixels;
}

} // namespace

// M5.4: multi-stage TEV chain (two textures + K constant) end-to-end.
TEST_CASE(vulkan_offscreen_gx_tev_quad) {
    // Configuration:
    //   stage 0: GXSetTevOp(REPLACE)  -> prev = texmap0 texel (solid red)
    //   stage 1: GXSetTevOp(REPLACE)  -> prev = texmap1 texel (solid green)
    //   stage 2: out = KONST (K0)     -> prev = (10,20,30,40)
    GXInit(nullptr, 0);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXSetTevOp(GX_TEVSTAGE1, GX_REPLACE);
    GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_K0_A);
    GXSetTevKColor(GX_KCOLOR0, GXColor{10, 20, 30, 40});
    GXSetNumTevStages(3);
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);

    TestContext ctx;
    if (!setupInstance(ctx)) {
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));

    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    SolidTex tex[2];
    createSolidTex(ctx, red, tex[0]);
    createSolidTex(ctx, green, tex[1]);
    VkImageView views[8] = {tex[0].view, tex[1].view, tex[0].view, tex[0].view,
                            tex[0].view, tex[0].view, tex[0].view, tex[0].view};

    TevFrameResources r;
    createTevFrameResources(ctx, &ubo, sizeof(ubo), views, r);

    const uint8_t* pixels = runTevQuadFrame(ctx, r, &ubo, sizeof(ubo));
    // The 8-bit output stage masks the chain result with 255 and the default
    // alpha compare (ALWAYS) passes; fog is off (GXInit default).
    for (const auto& xy : std::initializer_list<std::pair<uint32_t, uint32_t>>{
             {128u, 128u}, {96u, 96u}, {160u, 96u}, {96u, 160u}, {160u, 160u}}) {
        const uint8_t* p = pixels + (xy.second * TestContext::kWidth + xy.first) * 4;
        CHECK(std::abs(int(p[0]) - 10) <= 1);
        CHECK(std::abs(int(p[1]) - 20) <= 1);
        CHECK(std::abs(int(p[2]) - 30) <= 1);
        CHECK(std::abs(int(p[3]) - 40) <= 1);
    }
    vkUnmapMemory(ctx.device, ctx.readbackMemory);

    destroyTevFrameResources(ctx, r);
    for (auto& t : tex) {
        vkDestroyImageView(ctx.device, t.view, nullptr);
        vkDestroyImage(ctx.device, t.image, nullptr);
        vkFreeMemory(ctx.device, t.memory, nullptr);
    }
    teardown(ctx);
}

// M5.5: pixel-engine output stage (fog + alpha compare) end-to-end.
//
// The quad is drawn with the identity MVP at vertex z = 0.5, which lands on
// gl_FragCoord.z = 0.5 (Vulkan NDC depth [0,1]) and the shader's GX-reversed
// fog zCoord = int((1 - 0.5) * 2^24) = 8388608. With
// GXSetFog(GX_FOG_ORTHO_LIN, 2, 8, 1, 9, fog color (255,128,64)):
//   a = (9-1)/(8-2) = 8/6 -> register-truncated 1.3330078125
//   c = (2-1)/(8-2) = 1/6 -> register-truncated 0.1666259766
//   ze = 1.3330078125 * 8388608 / 16777216 = 0.66650390625
//   fog = 0.66650390625 - 0.1666259766 = 0.4998779297 -> ifog = round(·256) = 128
//   out = (prev·128 + fogcolor·128) >> 8 with prev = texel = (100,150,200,255)
//       = (177, 139, 132)
// The CPU reference (evalPixelEngine) computes the same formula.
TEST_CASE(vulkan_offscreen_gx_fog_alpha) {
    GXInit(nullptr, 0);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);   // prev = texmap0 texel
    GXSetFog(GX_FOG_ORTHO_LIN, 2.0f, 8.0f, 1.0f, 9.0f, GXColor{255, 128, 64, 255});
    GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);

    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    Platform::CompatGx::TevChainInputs in;
    in.texel[0][0] = 100; in.texel[0][1] = 150; in.texel[0][2] = 200; in.texel[0][3] = 255;

    // CPU reference at the same zCoord.
    bool discarded = false;
    std::int32_t cpuOut[4];
    Platform::CompatGx::evalPixelEngine(ubo, in, 8388608, discarded, cpuOut);
    REQUIRE(!discarded);
    CHECK_EQ(cpuOut[0], 177);
    CHECK_EQ(cpuOut[1], 139);
    CHECK_EQ(cpuOut[2], 132);
    CHECK_EQ(cpuOut[3], 255);

    TestContext ctx;
    if (!setupInstance(ctx)) {
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));

    const uint8_t texRgba[4] = {100, 150, 200, 255};
    SolidTex tex;
    createSolidTex(ctx, texRgba, tex);
    VkImageView views[8] = {tex.view, tex.view, tex.view, tex.view,
                            tex.view, tex.view, tex.view, tex.view};

    TevFrameResources r;
    createTevFrameResources(ctx, &ubo, sizeof(ubo), views, r);

    // Frame 1: alpha compare passes (255 > 128) -> fogged texel color.
    {
        const uint8_t* pixels = runTevQuadFrame(ctx, r, &ubo, sizeof(ubo));
        for (const auto& xy : std::initializer_list<std::pair<uint32_t, uint32_t>>{
                 {128u, 128u}, {96u, 96u}, {160u, 96u}, {96u, 160u}, {160u, 160u}}) {
            const uint8_t* p = pixels + (xy.second * TestContext::kWidth + xy.first) * 4;
            CHECK(std::abs(int(p[0]) - cpuOut[0]) <= 1);
            CHECK(std::abs(int(p[1]) - cpuOut[1]) <= 1);
            CHECK(std::abs(int(p[2]) - cpuOut[2]) <= 1);
            CHECK(std::abs(int(p[3]) - cpuOut[3]) <= 1);
        }
        vkUnmapMemory(ctx.device, ctx.readbackMemory);
    }

    // Frame 2: alpha compare fails (255 < 128) -> fragment discarded -> the
    // quad leaves the clear color (0.1, 0.1, 0.15, 1.0).
    {
        GXSetAlphaCompare(GX_LESS, 128, GX_AOP_AND, GX_ALWAYS, 0);
        Platform::CompatGx::buildTevUbo(ubo);
        const uint8_t* pixels = runTevQuadFrame(ctx, r, &ubo, sizeof(ubo));
        for (const auto& xy : std::initializer_list<std::pair<uint32_t, uint32_t>>{
                 {128u, 128u}, {96u, 96u}, {160u, 96u}, {96u, 160u}, {160u, 160u}}) {
            const uint8_t* p = pixels + (xy.second * TestContext::kWidth + xy.first) * 4;
            CHECK(std::abs(int(p[0]) - 25) <= 2);
            CHECK(std::abs(int(p[1]) - 25) <= 2);
            CHECK(std::abs(int(p[2]) - 38) <= 2);
            CHECK(std::abs(int(p[3]) - 255) <= 2);
        }
        vkUnmapMemory(ctx.device, ctx.readbackMemory);
    }

    destroyTevFrameResources(ctx, r);
    vkDestroyImageView(ctx.device, tex.view, nullptr);
    vkDestroyImage(ctx.device, tex.image, nullptr);
    vkFreeMemory(ctx.device, tex.memory, nullptr);
    teardown(ctx);
}

// M5.7a: channel lighting end-to-end through the real TEV shader.
//
// The GX channel state (GXSetChanCtrl/AmbColor/MatColor + GXLoadLightObjImm)
// drives the CPU per-vertex evaluator (applyChannelLighting — the same pure
// function the headless tests exercise), whose lit RGB/A bytes are written
// into clr0/clr1 of the fixed 27-float vertex layout. The TEV stage reads
// RASC/RASA (rasterizer color from the interpolated vertex attribute) and the
// framebuffer must come out with exactly the lit color. This is the first test
// that pushes a non-white vertex color through the kGxTev*Spv pipeline.
TEST_CASE(vulkan_offscreen_gx_chan_lighting) {
    // Same setup as the headless gx_chan_lighting_basic (TEST A):
    //   amb = (40,50,60,200), mat = (200,180,160,255),
    //   light0: pos (0,2,4), color (255,128,64,255), no attenuation,
    //   diffuse CLAMP. ldir = normalize(0,2,4); at the quad positions used
    //   below the per-vertex outputs are computed by applyChannelLighting
    //   itself (the mirror), so no hand-derived constants are needed.
    GXInit(nullptr, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0,
                  GX_DF_CLAMP, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{40, 50, 60, 200});
    GXSetChanMatColor(GX_COLOR0A0, GXColor{200, 180, 160, 255});
    GXLightObj light;
    GXInitLightPos(&light, 0.0f, 2.0f, 4.0f);
    GXInitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXInitLightColor(&light, GXColor{255, 128, 64, 255});
    GXLoadLightObjImm(&light, GX_LIGHT0);

    // TEV stage 0: out = RASC (+ RASA alpha) -> the lit vertex color. The
    // default TEV order already maps colorChan = GX_COLOR0A0 -> ras0 = clr0.
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_RASA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetNumTevStages(1);
    GXSetNumTexGens(0);

    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);

    // The quad's four distinct corners (triangle 1 = A,B,C; triangle 2 =
    // A,C,D), all with normal +Y, at view z = 0.5 (identity pos matrix).
    const float corners[4][3] = {
        {-0.6f, -0.6f, 0.5f}, {0.6f, -0.6f, 0.5f},
        {0.6f, 0.6f, 0.5f}, {-0.6f, 0.6f, 0.5f},
    };
    constexpr int kStride = 27;
    const int quadIndex[6] = {0, 1, 2, 0, 2, 3};
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float nrm[3] = {0.0f, 1.0f, 0.0f};
    float lit[4][4];
    for (int c = 0; c < 4; ++c) {
        float out0[4], out1[4];
        Platform::CompatGx::applyChannelLighting(corners[c], nrm, white, white,
                                                 out0, out1);
        for (int i = 0; i < 4; ++i) lit[c][i] = out0[i];
    }
    float quad[6 * kStride];
    for (int v = 0; v < 6; ++v) {
        float* d = quad + v * kStride;
        const int c = quadIndex[v];
        d[0] = corners[c][0]; d[1] = corners[c][1]; d[2] = corners[c][2];
        d[3] = lit[c][0]; d[4] = lit[c][1]; d[5] = lit[c][2]; d[6] = lit[c][3];
        d[7] = d[8] = d[9] = d[10] = 1.0f;         // clr1 unused by this TEV
        for (int t = 0; t < 8; ++t) { d[11 + 2*t] = 0.5f; d[12 + 2*t] = 0.5f; }
    }

    TestContext ctx;
    if (!setupInstance(ctx)) {
        SKIP("Vulkan not available (volkInitialize/instance failed)");
        return;
    }
    REQUIRE(setupOffscreen(ctx));

    SolidTex tex;
    const uint8_t black[4] = {0, 0, 0, 255};
    createSolidTex(ctx, black, tex);
    VkImageView views[8] = {tex.view, tex.view, tex.view, tex.view,
                            tex.view, tex.view, tex.view, tex.view};

    TevFrameResources r;
    createTevFrameResources(ctx, &ubo, sizeof(ubo), views, r, quad);

    const uint8_t* pixels = runTevQuadFrame(ctx, r, &ubo, sizeof(ubo));

    // Sample pixel (128, 96): inside triangle 1 (A, B, C). The quad's NDC span
    // [-0.6,0.6] maps to screen [51.2, 204.8] on the 256×256 offscreen surface;
    // with the Vulkan viewport convention (NDC y = −1 is the TOP, gl_FragCoord
    // y grows downward) the pixel center (128.5, 96.5) sits at NDC
    // (0.0039, −0.2461), below the A–C diagonal (y = x) and inside triangle 1.
    // The rasterizer interpolates the vertex colors affinely (z constant), so
    // the expected 8-bit value is the exact barycentric blend of the per-vertex
    // lit bytes:
    //   P = wA·A + wB·B + wC·C  with
    //   wB = (xc − yc)/1.2, wC = (1 − wB + (xc+yc)/1.2)/2, wA = (1 − wB − wC)
    // and the fragment shader converts with round(vColor0·255). Compare each
    // channel to round(Σ w·byte) with a ±2 tolerance for float rounding.
    const float xc = ((128.0f + 0.5f) / TestContext::kWidth) * 2.0f - 1.0f;
    const float yc = ((96.0f + 0.5f) / TestContext::kHeight) * 2.0f - 1.0f;
    const float wB = (xc - yc) / 1.2f;
    const float wC = (1.0f - wB + (xc + yc) / 1.2f) / 2.0f;
    const float wA = 1.0f - wB - wC;
    const uint8_t* p = pixels + (96 * TestContext::kWidth + 128) * 4;
    for (int ch = 0; ch < 3; ++ch) {
        const float expected =
            wA * lit[0][ch] * 255.0f + wB * lit[1][ch] * 255.0f +
            wC * lit[2][ch] * 255.0f;
        CHECK(std::abs(int(p[ch]) - static_cast<int>(expected + 0.5f)) <= 2);
    }
    // RASA alpha: all lit alphas are 255 -> interpolated 255.
    CHECK(std::abs(int(p[3]) - 255) <= 2);

    vkUnmapMemory(ctx.device, ctx.readbackMemory);
    destroyTevFrameResources(ctx, r);
    vkDestroyImageView(ctx.device, tex.view, nullptr);
    vkDestroyImage(ctx.device, tex.image, nullptr);
    vkFreeMemory(ctx.device, tex.memory, nullptr);
    teardown(ctx);
}
