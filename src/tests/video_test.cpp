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

#include "platform/Renderer/RendererCommon.h"
#include "platform/Renderer/vk_demo_shaders.h"

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
