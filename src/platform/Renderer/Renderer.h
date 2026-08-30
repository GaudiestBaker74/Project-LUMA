#pragma once
// =============================================================================
// Platform::Renderer — closed rendering API (Milestone 4).
//
// This is THE graphics layer of the port: the thin, explicit bridge between
// compat/gx and Vulkan (docs/renderer.md). It owns the Vulkan instance,
// device, swapchain, present path, pipeline cache and per-frame command
// recording. There is deliberately no "generic backend" interface: renderer.h
// IS the API and vulkan/ is the only backend (YAGNI — a D3D12 backend would
// introduce the virtual interface at that point).
//
// M4.1 scope: device/swapchain/present (moved from M3's Video), frame/pass
// control, vertex buffers, pipeline cache keyed by state hash, uniforms via
// push constants, draw/drawIndexed. Textures/samplers/render targets land in
// M4.2; the EFB (offscreen 640x448/576 render target) becomes real in M5
// when GXCopyDisp needs it.
//
// Handles are opaque void* — the public header never depends on volk/Vulkan.
// =============================================================================

#include "platform/Renderer/FrameStats.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct SDL_Window;

namespace Platform {

// Internal: backend memory helper (defined in Renderer.cpp). Forward-declared
// here (outside the class) so the friend declaration names the same entity;
// not part of the public API.
struct RendererAccess;


struct RendererConfig {
    const char* appName = "galaxy-pc";
    bool enableValidation = false; // VK_LAYER_KHRONOS_validation (+ debug utils)
    bool vsync = true;             // VK_PRESENT_MODE_FIFO_KHR vs IMMEDIATE
    int swapchainWidth = 0;        // 0 = window size at init
    int swapchainHeight = 0;
};

// Clear color + depth/stencil for beginPass(). Colors are in [0,1].
struct ClearValue {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    float depth = 1.0f;
    uint32_t stencil = 0;
};

// Vertex attribute format. Mirrors the GX attribute types the port needs
// (docs/gx.md §3.B); R8G8B8A8_UNORM/B8G8R8A8_UNORM map to Vulkan directly,
// the packed GX-only ones are unpacked by compat/gx before upload.
enum class VertexFormat : uint8_t {
    R32G32_SFLOAT,
    R32G32B32_SFLOAT,
    R32G32B32A32_SFLOAT,
    R8G8B8A8_UNORM,
    R8G8B8A8_UINT,
    R16G16_UINT,
    R16G16_UNORM,
    // Color attachment formats (also valid as vertex formats? no — color only;
    // listed here because PipelineDesc::colorFormat reuses this enum).
    B8G8R8A8_UNORM,
};

struct VertexAttrib {
    uint32_t location = 0;   // shader location
    uint32_t offset = 0;     // byte offset within the vertex
    VertexFormat format = VertexFormat::R32G32B32_SFLOAT;

    bool operator==(const VertexAttrib&) const = default;
};

struct VertexLayout {
    uint32_t stride = 0;                    // bytes per vertex
    std::vector<VertexAttrib> attribs;      // sorted by location
};

enum class PrimitiveTopology : uint8_t { TriangleList, TriangleStrip, LineList };

enum class BufferUsage : uint8_t {
    Vertex,
    Index,
    Uniform,   // UBO (M4.2+; M4.1 uses push constants)
    Staging,
};

// Texture formats. Color formats are the ones compat/gx produces after any
// CPU unpacking (see Format.h for the GX->TextureFormat strategy); depth
// formats map the GX Z formats (Z24X8 -> D24_UNORM_S8_UINT).
enum class TextureFormat : uint8_t {
    R8G8B8A8_UNORM,
    B8G8R8A8_UNORM,
    R8G8B8A8_SRGB,
    R16G16B16A16_FLOAT,
    R32G32B32A32_FLOAT,
    D16_UNORM,
    D24_UNORM_S8_UINT, // GX Z24X8 (stencil byte unused)
    D32_SFLOAT,
};

enum class SamplerFilter : uint8_t { Nearest, Linear };
enum class SamplerAddressMode : uint8_t { Repeat, ClampToEdge, MirrorRepeat };

struct SamplerDesc {
    SamplerFilter magFilter = SamplerFilter::Linear;
    SamplerFilter minFilter = SamplerFilter::Linear;
    SamplerFilter mipFilter = SamplerFilter::Linear;
    SamplerAddressMode addressU = SamplerAddressMode::ClampToEdge;
    SamplerAddressMode addressV = SamplerAddressMode::ClampToEdge;
    bool operator==(const SamplerDesc&) const = default;
};

struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    TextureFormat format = TextureFormat::R8G8B8A8_UNORM;
    // Row-major RGBA8 (or the format's natural layout) host bytes uploaded via
    // a staging buffer. Null = undefined contents (upload later / fill via
    // render).
    const void* initialData = nullptr;
    const char* debugName = nullptr; // debug labels (--gpu-debug)
};

// Offscreen render target: the M4.2 "EFB" (the game renders at 640x448/576
// and GXCopyDisp blits it to the swapchain — wired in M5). Depth optional.
struct RenderTargetDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    TextureFormat colorFormat = TextureFormat::R8G8B8A8_UNORM;
    bool hasDepth = true;
    TextureFormat depthFormat = TextureFormat::D24_UNORM_S8_UINT; // GX Z24X8
    const char* debugName = nullptr;
};

// Full state needed to build a graphics pipeline. compat/gx fills the union of
// GX state per draw (TEV/blend/depth/cull...); M4.1 only consumes the fields
// below (the rest default and are documented for M5).
struct PipelineDesc {
    VertexLayout vertexLayout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;

    // Embedded SPIR-V (see tools/compile_shaders.sh).
    const uint32_t* vertSpv = nullptr;
    size_t vertSpvSize = 0;
    const uint32_t* fragSpv = nullptr;
    size_t fragSpvSize = 0;

    // Rasterization (GX-equivalents; defaults are the M3 demo's).
    bool blendEnable = false;          // GXSetBlendMode(... GX_BM_BLEND ...)
    bool depthTest = false;            // GXSetZMode
    bool depthWrite = false;
    bool cullBack = false;             // GXSetCullMode (front faces are CCW)

    // Color format of the attachment this pipeline renders into (required for
    // dynamic rendering). Set by the caller from the pass's target format.
    VertexFormat colorFormat = VertexFormat::R8G8B8A8_UNORM;

    // Number of combined image samplers the fragment shader expects in
    // set 0, binding 0 (array). 0 = shader has no textures. bindTexture()
    // writes element `binding` of that array.
    uint32_t textureCount = 0;

    bool operator==(const PipelineDesc&) const; // for the cache
};

using PipelineHandle = void*;     // owned by the cache (never freed individually)
using BufferHandle = void*;       // opaque VkBuffer + memory
using TextureHandle = void*;      // opaque VkImage + view + memory
using SamplerHandle = void*;      // owned by the cache (never freed individually)
using RenderTargetHandle = void*; // opaque color(+depth) images + views

// --- public API -------------------------------------------------------------
class Renderer {
public:
    // Lifecycle. Requires an existing SDL window. Returns false on fatal
    // setup failure. Idempotent after shutdown(). There is a single
    // renderer instance (like the console had a single VI/GX).
    static bool init(SDL_Window* window, const RendererConfig& config);
    static void shutdown();
    static Renderer& instance();

    bool isInitialized() const { return mDevice != nullptr; }

    // --- frame --------------------------------------------------------------
    // Acquires the next swapchain image (waits on the previous frame's
    // fence). Returns false when the swapchain was recreated / out of date —
    // the caller should skip rendering this tick.
    bool beginFrame();
    // Presents. Returns false if the window was destroyed.
    void endFrame();
    void waitIdle();

    // Called by the window resize path; recreates the swapchain.
    void onResize(int width, int height);

    // --- frame statistics (M4.3) -------------------------------------------
    // Stats of the last completed frame: CPU time in the render path, GPU time
    // (VK_QUERY_TYPE_TIMESTAMP; 0 if unsupported) and device-local memory
    // budget (VK_EXT_memory_budget; 0 if unavailable).
    const FrameStats& lastFrameStats() const { return mLastFrameStats; }

    // --- pass ---------------------------------------------------------------
    // Begins rendering into the swapchain image (the M3 "EFB" for now).
    // Clears with `clear`. The no-arg form uses the color stored by
    // setClearColor() (the compat/gx path: GXClearColor -> GXClear).
    void beginPass();
    void beginPass(const ClearValue& clear);
    void endPass();

    // GX hook (compat/gx, M3 heritage): stores the clear color used by the
    // no-arg beginPass(). Replaced by the GX state mirror in M5.
    void setClearColor(float r, float g, float b, float a);

    void setViewport(float x, float y, float w, float h);
    void setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // --- resources (M4.1: buffers) -----------------------------------------
    BufferHandle createBuffer(BufferUsage usage, uint64_t size, const void* initialData);
    void destroyBuffer(BufferHandle buffer);
    void bindVertexBuffer(BufferHandle buffer, uint64_t offset);
    void bindIndexBuffer(BufferHandle buffer, uint64_t offset);

    // --- resources (M4.2: textures / samplers / render targets) -------------
    // Uploads `initialData` via a staging buffer (device-local image).
    TextureHandle createTexture(const TextureDesc& desc);
    void destroyTexture(TextureHandle texture);

    // Cached by SamplerDesc (identical descs share one VkSampler).
    SamplerHandle getOrCreateSampler(const SamplerDesc& desc);

    // Binds texture `binding` (array element of set 0) for the currently bound
    // pipeline. Valid only while a textured pipeline (textureCount > binding)
    // is bound and inside a pass. TODO(PC_PORT): per-TEX-stage samplers from
    // GX arrive in M5; M4.2 applies one sampler to the binding.
    void bindTexture(uint32_t binding, TextureHandle texture, SamplerHandle sampler);

    // Offscreen target (the future EFB). beginPass(RT) renders into it
    // instead of the swapchain image; pipelines must be created with
    // PipelineDesc::colorFormat matching the target's colorFormat.
    RenderTargetHandle createRenderTarget(const RenderTargetDesc& desc);
    void destroyRenderTarget(RenderTargetHandle target);
    void beginPass(RenderTargetHandle target);
    void beginPass(RenderTargetHandle target, const ClearValue& clear);

    // --- pipeline cache -----------------------------------------------------
    // Returns a cached pipeline for `desc` (hash key); identical descs share
    // one Vulkan pipeline. Shaders are compiled at first use.
    PipelineHandle getOrCreatePipeline(const PipelineDesc& desc);
    void bindPipeline(PipelineHandle pipeline);

    // --- uniforms (M4.1: push constants, vertex stage) ----------------------
    // Binds a small per-draw uniform block (≤ 128 bytes). compat/gx packs
    // matrices/colors here; the demo pushes the 2D rotation matrix.
    void setUniforms(const void* data, uint32_t size);

    // --- draw ---------------------------------------------------------------
    void draw(uint32_t vertexCount, uint32_t firstVertex);
    void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset);

    // Color format of the current pass attachment (the swapchain image for
    // M4.1). Pipelines must be created with this as PipelineDesc::colorFormat.
    VertexFormat passColorFormat() const;

private:
    Renderer() = default;
    ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Backend helpers in Renderer.cpp that need the private Vulkan state.
    // Keeps volk types out of this header.
    friend struct RendererAccess;

    // --- Vulkan state (opaque; real types live in Renderer.cpp) -------------
    void* mInstance = nullptr;           // VkInstance
    void* mDebugMessenger = nullptr;     // VkDebugUtilsMessengerEXT (--gpu-debug)
    void* mSurface = nullptr;            // VkSurfaceKHR
    void* mWindow = nullptr;             // SDL_Window* (owned by Platform::Window)
    void* mPhysicalDevice = nullptr;     // VkPhysicalDevice
    void* mDevice = nullptr;             // VkDevice
    void* mGraphicsQueue = nullptr;      // VkQueue
    void* mPresentQueue = nullptr;       // VkQueue
    uint32_t mGraphicsFamily = 0;
    uint32_t mPresentFamily = 0;

    void* mSwapchain = nullptr;          // VkSwapchainKHR
    uint32_t mImageCount = 0;
    uint32_t mImageIndex = 0;
    uint32_t mSwapchainFormat = 0;       // VkFormat
    uint32_t mExtentW = 0;
    uint32_t mExtentH = 0;

    void* mCommandPool = nullptr;        // VkCommandPool (persistent, M4.1)
    void* mCmd = nullptr;                // VkCommandBuffer (current frame)
    void* mPassImageView = nullptr;      // VkImageView of the acquired swapchain image
    void* mDescriptorPool = nullptr;     // VkDescriptorPool (M4.2, textured pipelines)
    void* mPassTarget = nullptr;         // GpuRenderTarget* of the current pass, or nullptr (swapchain)
    void* mFence = nullptr;              // VkFence (frame in flight)
    void* mImageAvailable = nullptr;     // VkSemaphore (acquire -> submit)
    std::vector<void*> mRenderFinished;  // VkSemaphore[imageCount] (submit -> present)

    float mClearR = 0.1f, mClearG = 0.1f, mClearB = 0.15f, mClearA = 1.0f;

    // M4.3 frame statistics.
    void* mQueryPool = nullptr;        // VkQueryPool (2 timestamps: frame start/end)
    float mTimestampPeriod = 1.0f;     // ns per GPU timestamp tick
    bool mHasTimestamps = false;       // device supports timestampComputeAndGraphics
    bool mHasMemoryBudget = false;     // VK_EXT_memory_budget available + enabled
    double mCpuPhaseStart = 0.0;       // Timing::nowSeconds() at render-phase start
    FrameStats mLastFrameStats;

    bool mValidation = false;
    bool mVsync = true;
    bool mInPass = false;
    bool mSwapchainOutOfDate = false;

    // --- internals (used by Renderer.cpp) -----------------------------------
    struct PipelineCacheEntry {
        PipelineDesc desc;           // full desc (collision check)
        void* pipeline;              // VkPipeline
        void* layout;                // VkPipelineLayout (push constants + set 0)
        void* descriptorSetLayout;   // VkDescriptorSetLayout (textured pipelines only)
        void* descriptorSet;         // VkDescriptorSet (one per textured pipeline)
    };
    struct SamplerCacheEntry {
        SamplerDesc desc;
        void* sampler;               // VkSampler
    };
    std::unordered_map<uint64_t, PipelineCacheEntry> mPipelineCache;
    std::unordered_map<void*, PipelineCacheEntry*> mPipelineByHandle;
    std::unordered_map<uint64_t, SamplerCacheEntry> mSamplerCache;
    PipelineCacheEntry* mBoundEntry = nullptr; // current pipeline's cache entry

    void destroySwapchainInternal();
    void recreateSwapchainInternal();
    void updateMemoryBudget();
    // (Opaque params: the public header must not depend on volk/Vulkan types.)
    void recordBeginPass(void* colorView, void* depthView, bool hasDepth,
                         uint32_t w, uint32_t h, const ClearValue& clear);
    void setDebugName(uint32_t objectType, uint64_t handle, const char* name);
};

} // namespace Platform
