#pragma once
// =============================================================================
// Platform::RenderDetail — format conversion tables.
//
// Pure mappings (no device state) so they are unit-testable headless. The
// GX-side formats arrive with compat/gx (M5): those functions are stubbed
// with TODO(PC_PORT) until the inventory is wired in.
// =============================================================================

#include "platform/Renderer/Renderer.h"

#include "volk.h"

namespace Platform::RenderDetail {

// Vertex attribute formats (Platform::VertexFormat) -> VkFormat.
inline VkFormat vertexFormatToVk(Platform::VertexFormat fmt) {
    using VF = Platform::VertexFormat;
    switch (fmt) {
        case VF::R32G32_SFLOAT:    return VK_FORMAT_R32G32_SFLOAT;
        case VF::R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case VF::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VF::R8G8B8A8_UNORM:   return VK_FORMAT_R8G8B8A8_UNORM;
        case VF::R8G8B8A8_UINT:    return VK_FORMAT_R8G8B8A8_UINT;
        case VF::R16G16_UINT:      return VK_FORMAT_R16G16_UINT;
        case VF::R16G16_UNORM:     return VK_FORMAT_R16G16_UNORM;
        case VF::B8G8R8A8_UNORM:   return VK_FORMAT_UNDEFINED; // color-only
    }
    return VK_FORMAT_UNDEFINED;
}

// Color attachment formats used by the swapchain and render targets.
inline VkFormat colorFormatToVk(Platform::VertexFormat fmt) {
    switch (fmt) {
        case Platform::VertexFormat::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case Platform::VertexFormat::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case Platform::VertexFormat::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

// Texture formats (Platform::TextureFormat) -> VkFormat.
inline VkFormat textureFormatToVk(Platform::TextureFormat fmt) {
    using TF = Platform::TextureFormat;
    switch (fmt) {
        case TF::R8G8B8A8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
        case TF::B8G8R8A8_UNORM:       return VK_FORMAT_B8G8R8A8_UNORM;
        case TF::R8G8B8A8_SRGB:        return VK_FORMAT_R8G8B8A8_SRGB;
        case TF::R16G16B16A16_FLOAT:   return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TF::R32G32B32A32_FLOAT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
        case TF::D16_UNORM:            return VK_FORMAT_D16_UNORM;
        case TF::D24_UNORM_S8_UINT:    return VK_FORMAT_D24_UNORM_S8_UINT;
        case TF::D32_SFLOAT:           return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

// Sampler state -> VkFilter / VkSamplerAddressMode.
inline VkFilter filterToVk(Platform::SamplerFilter f) {
    return f == Platform::SamplerFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}
inline VkSamplerMipmapMode mipFilterToVk(Platform::SamplerFilter f) {
    return f == Platform::SamplerFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                 : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}
inline VkSamplerAddressMode addressModeToVk(Platform::SamplerAddressMode m) {
    switch (m) {
        case Platform::SamplerAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case Platform::SamplerAddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case Platform::SamplerAddressMode::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

// GX texture format -> strategy + VkFormat.
//
// Native-mapped (upload the GX block layout converted to a Vulkan format):
//   GX_TF_I4/I8      -> R8_UNORM        (luminance; convert to RGBA8 or R8)
//   GX_TF_IA4/IA8    -> R8G8_UNORM      (intensity+alpha)
//   GX_TF_RGB565     -> R5G6B5_UNORM    (Vulkan has it; cheap)
//   GX_TF_RGB5A3     -> unpacked to RGBA8 on CPU (no 5a3 format in Vulkan)
//   GX_TF_RGBA8      -> R8G8B8A8_UNORM  (native-ish; GX stores as two planes)
//   GX_TF_CMPR (DXT1) -> BC1_RGBA_UNORM_BLOCK (native; 4x4 blocks)
//   GX_TF_CI4/CI8 (paletted) -> resolved on CPU to RGBA8 by compat/gx
// Depth:
//   GX_TF_Z24X8 / GX_Z24X8 -> D24_UNORM_S8_UINT (stencil byte unused)
//   GX_TF_Z16          -> D16_UNORM
//
// M4.2 implements the Vulkan side (TextureFormat). The GX-side table lives in
// compat/gx (M5): `textureFormatFromGx` below is the seam where the GX
// texture format enum maps onto Platform::TextureFormat.
// TODO(PC_PORT, M5): fill the GX->TextureFormat table when the GX texture
// inventory is wired in (GXTexFmt/GXTexZ16 etc.).
inline Platform::TextureFormat textureFormatFromGx(uint8_t /*gxTexFmt*/) {
    return Platform::TextureFormat::R8G8B8A8_UNORM; // safe default; refined in M5
}

} // namespace Platform::RenderDetail
