// =============================================================================
// Platform::Renderer format conversion tables (pure, no Vulkan runtime needed).
// =============================================================================

#include "tests/test_runner.h"

#include "platform/Renderer/Format.h"
#include "platform/Renderer/Renderer.h"

#include <cstring>

TEST_CASE(renderer_vertex_format_mapping) {
    using VF = Platform::VertexFormat;

    // Every vertex format maps to a distinct, valid VkFormat.
    const VF all[] = {
        VF::R32G32_SFLOAT, VF::R32G32B32_SFLOAT, VF::R32G32B32A32_SFLOAT,
        VF::R8G8B8A8_UNORM, VF::R8G8B8A8_UINT, VF::R16G16_UINT, VF::R16G16_UNORM,
    };
    for (const VF fmt : all) {
        const VkFormat vk = Platform::RenderDetail::vertexFormatToVk(fmt);
        CHECK(vk != VK_FORMAT_UNDEFINED);
    }

    // Spot checks against the Vulkan enum values.
    CHECK(Platform::RenderDetail::vertexFormatToVk(VF::R32G32_SFLOAT) == VK_FORMAT_R32G32_SFLOAT);
    CHECK(Platform::RenderDetail::vertexFormatToVk(VF::R32G32B32_SFLOAT) == VK_FORMAT_R32G32B32_SFLOAT);
    CHECK(Platform::RenderDetail::vertexFormatToVk(VF::R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(Platform::RenderDetail::vertexFormatToVk(VF::R16G16_UNORM) == VK_FORMAT_R16G16_UNORM);

    // B8G8R8A8 is a color attachment format, not a vertex format.
    CHECK(Platform::RenderDetail::vertexFormatToVk(VF::B8G8R8A8_UNORM) == VK_FORMAT_UNDEFINED);
}

TEST_CASE(renderer_color_format_mapping) {
    using VF = Platform::VertexFormat;

    CHECK(Platform::RenderDetail::colorFormatToVk(VF::R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(Platform::RenderDetail::colorFormatToVk(VF::B8G8R8A8_UNORM) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(Platform::RenderDetail::colorFormatToVk(VF::R32G32B32A32_SFLOAT) ==
          VK_FORMAT_R32G32B32A32_SFLOAT);

    // The demo pass format must round-trip: R8G8B8A8 and B8G8R8A8 are the two
    // swapchain formats the renderer reports via passColorFormat().
    CHECK(Platform::RenderDetail::colorFormatToVk(VF::R8G8B8A8_UNORM) != VK_FORMAT_UNDEFINED);
}

TEST_CASE(renderer_pipeline_desc_equality) {
    using VF = Platform::VertexFormat;
    Platform::PipelineDesc a;
    a.vertexLayout.stride = 20;
    a.vertexLayout.attribs = {{0, 0, VF::R32G32_SFLOAT}, {1, 8, VF::R32G32B32_SFLOAT}};
    a.colorFormat = VF::R8G8B8A8_UNORM;

    Platform::PipelineDesc b = a;
    CHECK(a == b);

    b.blendEnable = true;
    CHECK(!(a == b));

    b = a;
    b.vertexLayout.attribs[1].offset = 12;
    CHECK(!(a == b));
}

// PC_PORT M9.5.4: the pipeline-cache key must depend only on the pipeline
// state — never on struct padding or per-draw values. The old FNV mixed the
// raw 12 bytes of each VertexAttrib (3 of them padding): on MSVC the padding
// of the per-draw braced-init attribute list held stack garbage, so the title
// screen minted one Vulkan pipeline per frame (boot.log `pipeline cache insert
// #10..#1108`) until the descriptor pool ran dry and every draw failed with
// "no textured pipeline bound".
TEST_CASE(renderer_pipeline_desc_hash_ignores_padding_and_dynamic_state) {
    using VF = Platform::VertexFormat;

    Platform::PipelineDesc a;
    a.vertexLayout.stride = 20;
    a.vertexLayout.attribs = {{0, 0, VF::R32G32B32_SFLOAT}, {1, 12, VF::R32G32_SFLOAT}};
    a.textureCount = 8;
    a.fragmentUbo = true;

    // b: identical fields, but every byte of the attribute storage (padding
    // included) poisoned first — exactly what an uninitialised stack
    // initializer_list array looks like on MSVC. VertexAttrib is an
    // implicit-lifetime aggregate, so memset + member stores are well-defined.
    Platform::PipelineDesc b = a;
    static_assert(sizeof(Platform::VertexAttrib) > 2 * sizeof(uint32_t) + sizeof(VF),
                  "VertexAttrib has no padding any more — adapt this test");
    std::memset(b.vertexLayout.attribs.data(), 0xA5,
                b.vertexLayout.attribs.size() * sizeof(Platform::VertexAttrib));
    for (size_t i = 0; i < a.vertexLayout.attribs.size(); ++i) {
        b.vertexLayout.attribs[i].location = a.vertexLayout.attribs[i].location;
        b.vertexLayout.attribs[i].offset = a.vertexLayout.attribs[i].offset;
        b.vertexLayout.attribs[i].format = a.vertexLayout.attribs[i].format;
    }
    // (defaulted operator== compares fields only — the descs compare equal)
    CHECK(a == b);
    CHECK_EQ(a.hash(), b.hash());

    // Real state changes DO change the key.
    Platform::PipelineDesc c = a;
    c.vertexLayout.attribs[1].offset = 16;
    CHECK(a.hash() != c.hash());
    c = a;
    c.vertexLayout.attribs.pop_back();
    CHECK(a.hash() != c.hash());
    c = a;
    c.blendEnable = true;
    CHECK(a.hash() != c.hash());
    c = a;
    c.dstAlphaEnable = true;
    CHECK(a.hash() != c.hash());
    c = a;
    c.depthCompare = Platform::CompareOp::Always;
    CHECK(a.hash() != c.hash());
    c = a;
    c.cullMode = Platform::CullMode::Back;
    CHECK(a.hash() != c.hash());
    c = a;
    c.textureCount = 0;
    CHECK(a.hash() != c.hash());
    c = a;
    c.depthFormat = Platform::TextureFormat::D24_UNORM_S8_UINT;
    CHECK(a.hash() != c.hash());

    // The key is a pure function of the fields (stable across calls).
    CHECK_EQ(a.hash(), a.hash());
}

TEST_CASE(renderer_texture_format_mapping) {
    using TF = Platform::TextureFormat;

    // Every texture format maps to a valid, distinct VkFormat.
    const TF all[] = {
        TF::R8G8B8A8_UNORM, TF::B8G8R8A8_UNORM, TF::R8G8B8A8_SRGB,
        TF::R16G16B16A16_FLOAT, TF::R32G32B32A32_FLOAT,
        TF::D16_UNORM, TF::D24_UNORM_S8_UINT, TF::D32_SFLOAT,
    };
    for (const TF fmt : all) {
        CHECK(Platform::RenderDetail::textureFormatToVk(fmt) != VK_FORMAT_UNDEFINED);
    }

    // Spot checks.
    CHECK(Platform::RenderDetail::textureFormatToVk(TF::R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(Platform::RenderDetail::textureFormatToVk(TF::B8G8R8A8_UNORM) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(Platform::RenderDetail::textureFormatToVk(TF::R8G8B8A8_SRGB) == VK_FORMAT_R8G8B8A8_SRGB);
    // GX Z24X8 maps to D24_UNORM_S8_UINT (stencil byte unused).
    CHECK(Platform::RenderDetail::textureFormatToVk(TF::D24_UNORM_S8_UINT) == VK_FORMAT_D24_UNORM_S8_UINT);
}

TEST_CASE(renderer_sampler_mapping) {
    using SF = Platform::SamplerFilter;
    using SA = Platform::SamplerAddressMode;

    CHECK(Platform::RenderDetail::filterToVk(SF::Nearest) == VK_FILTER_NEAREST);
    CHECK(Platform::RenderDetail::filterToVk(SF::Linear) == VK_FILTER_LINEAR);
    CHECK(Platform::RenderDetail::addressModeToVk(SA::Repeat) == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    CHECK(Platform::RenderDetail::addressModeToVk(SA::ClampToEdge) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CHECK(Platform::RenderDetail::addressModeToVk(SA::MirrorRepeat) == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
}

TEST_CASE(renderer_sampler_desc_equality) {
    Platform::SamplerDesc a;
    Platform::SamplerDesc b = a;
    CHECK(a == b);
    b.minFilter = Platform::SamplerFilter::Nearest;
    CHECK(!(a == b));
    b = a;
    b.addressU = Platform::SamplerAddressMode::Repeat;
    CHECK(!(a == b));
}

TEST_CASE(renderer_frame_stats_conversion) {
    // 1 tick * 1 ns/tick = 1e-6 ms.
    CHECK_NEAR(Platform::gpuTicksToMs(1, 1.0f), 1e-6, 1e-12);
    // 1e6 ticks * 1 ns/tick = 1 ms.
    CHECK_NEAR(Platform::gpuTicksToMs(1000000, 1.0f), 1.0, 1e-9);
    // 16.6 ms at a typical GPU timestamp period (1000 ns/tick).
    CHECK_NEAR(Platform::gpuTicksToMs(16600, 1000.0f), 16.6, 0.01);
    // Zero delta = zero time.
    CHECK_NEAR(Platform::gpuTicksToMs(0, 250.0f), 0.0, 1e-12);
}
