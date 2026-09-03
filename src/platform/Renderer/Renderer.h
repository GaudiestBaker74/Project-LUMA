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
    Undefined,          // no vertex attribute / no depth attachment
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

enum class PrimitiveTopology : uint8_t {
    TriangleList,
    TriangleStrip,
    TriangleFan,  // GX_TRIANGLEFAN (J3D shapes)
    LineList,     // GX_LINES (pairs)
    LineStrip,    // GX_LINESTRIP
    PointList,    // GX_POINTS
};

// Fixed-function state enums (GX pixel-engine semantics, mapped to Vulkan in
// Renderer.cpp — see src/compat/gx/GXCompat.cpp for the GX->here mapping).
enum class BlendFactor : uint8_t {
    Zero,
    One,
    SrcColor,        // the fragment's output color (GX_BL_SRCCLR on either slot)
    OneMinusSrcColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,        // the current framebuffer alpha (or the dst-alpha constant)
    OneMinusDstAlpha,
    ConstantAlpha,   // used when GXSetDstAlpha is enabled (both slots)
    OneMinusConstantAlpha,
};
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract };
enum class CompareOp : uint8_t {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always
};
enum class CullMode : uint8_t { None, Back, Front, FrontAndBack };
// Logic ops — numeric values are 1:1 with VkLogicOp (and with GXLogicOp).
enum class LogicOp : uint8_t {
    Clear = 0, And, AndReverse, Copy, AndInverted, NoOp, Xor, Or, Nor, Equiv,
    Invert, OrReverse, CopyInverted, OrInverted, Nand, Set
};

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
    Undefined = 0,       // no attachment / no depth in the pass
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
    // GXSetBlendMode: blend on/off, factors, op (SUBTRACT = dst − src), logic
    // op (GX_BM_LOGIC). With logicOpEnable the color attachment uses the
    // logic op instead of blending.
    bool blendEnable = false;
    BlendFactor srcBlendFactor = BlendFactor::SrcAlpha;
    BlendFactor dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
    BlendOp blendOp = BlendOp::Add;
    bool logicOpEnable = false;
    LogicOp logicOp = LogicOp::Copy;
    // GXSetDstAlpha: when enabled, the DSTALPHA/INVDSTALPHA factors read the
    // constant alpha value instead of the framebuffer alpha.
    bool dstAlphaEnable = false;
    float dstAlphaValue = 0.0f;
    // GXSetZMode / GXSetZCompLoc.
    bool depthTest = false;            // GXSetZMode(compare_enable, ...)
    bool depthWrite = false;           // GXSetZMode(..., update_enable)
    CompareOp depthCompare = CompareOp::LessEqual;
    // GXSetCullMode (front faces are CCW).
    CullMode cullMode = CullMode::None;
    // GXSetColorUpdate / GXSetAlphaUpdate -> color write masks.
    bool colorWrite = true;
    bool alphaWrite = true;

    // Color format of the attachment this pipeline renders into (required for
    // dynamic rendering). Set by the caller from the pass's target format.
    VertexFormat colorFormat = VertexFormat::R8G8B8A8_UNORM;
    // Depth attachment format for dynamic rendering; Undefined when the
    // pipeline must not touch depth (the pass has none — then depth test/write
    // are forced off at pipeline creation). The GX path sets this from
    // Renderer::passDepthFormat().
    TextureFormat depthFormat = TextureFormat::Undefined;

    // Number of combined image samplers the fragment shader expects in
    // set 0, binding 0 (array). 0 = shader has no textures. bindTexture() /
    // bindFragmentTextures() write elements of that array.
    uint32_t textureCount = 0;

    // The fragment shader reads a TEV-style std140 UBO in set 1, binding 0
    // (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC). Uploaded per draw with
    // uploadFragmentUbo() (M5.4).
    bool fragmentUbo = false;

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

    // True between beginPass()/beginPass(RT) and endPass().
    bool inPass() const { return mInPass; }

    void setViewport(float x, float y, float w, float h);
    void setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // --- resources (M4.1: buffers) -----------------------------------------
    BufferHandle createBuffer(BufferUsage usage, uint64_t size, const void* initialData);
    void destroyBuffer(BufferHandle buffer);
    void bindVertexBuffer(BufferHandle buffer, uint64_t offset);
    void bindIndexBuffer(BufferHandle buffer, uint64_t offset);

    // --- resources (M5.2: dynamic vertex buffer) ----------------------------
    // Host-visible vertex buffer reused across primitives and frames (the GX
    // vertex accumulator). The handle stays valid across internal growth; the
    // previous allocation is retired and destroyed at the next endFrame()
    // after the frame fence, so growing while draws recorded this frame still
    // reference the old allocation is safe.
    BufferHandle createDynamicBuffer(uint64_t initialSize);
    // Grows the buffer so `minBytes` fit (preserving existing contents).
    // Returns false on allocation failure (buffer left untouched).
    bool ensureBufferCapacity(BufferHandle buffer, uint64_t minBytes);
    // Copies `size` bytes into the dynamic buffer at byte `offset` (host
    // memory, coherent; bounds-checked against capacity).
    bool updateDynamicBuffer(BufferHandle buffer, uint64_t offset, uint64_t size,
                             const void* data);

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

    // M5.4 (TEV): writes `count` textures into set 0, binding 0 (elements
    // 0..count-1) and binds the set once. `tex[i]`/`sam[i]` must not be null.
    // Call after bindPipeline, before draw; only valid for pipelines with
    // textureCount >= count.
    void bindFragmentTextures(const TextureHandle* tex, const SamplerHandle* sam,
                              uint32_t count);

    // Offscreen target (the future EFB). beginPass(RT) renders into it
    // instead of the swapchain image; pipelines must be created with
    // PipelineDesc::colorFormat matching the target's colorFormat.
    RenderTargetHandle createRenderTarget(const RenderTargetDesc& desc);
    void destroyRenderTarget(RenderTargetHandle target);
    void beginPass(RenderTargetHandle target);
    void beginPass(RenderTargetHandle target, const ClearValue& clear);

    // --- M5.7c: EFB copy / present -----------------------------------------
    // Blits the color attachment of the render target most recently rendered
    // (last beginPass(target)/endPass()) into the current swapchain image,
    // scaling to the window size, and transitions it to PRESENT_SRC for the
    // upcoming endFrame()/present. Call after endPass(), before endFrame().
    // Returns false when there is no offscreen target to blit (the last pass
    // was the swapchain itself) or the renderer is not initialized.
    bool blitPassToSwapchain();

    // Submits the frame command buffer recorded so far and reopens a fresh
    // one, so that a later synchronous readback (readRenderTarget) sees the
    // content drawn before this call. The frame's present still happens at
    // endFrame() (the second submission). Required before reading back a
    // pass that was just rendered but not yet submitted — GXCopyTex between
    // passes (docs/gx.md §J). No-op (returns false) when no frame is open.
    bool flushFrame();

    // Synchronously reads back a region of `target`'s color attachment into
    // `outRgba8` (w*h*4 bytes, R8G8B8A8, converted from the target format).
    // Runs on its own one-shot command buffer + staging buffer and waits for
    // the GPU (vkQueueWaitIdle), so it is safe to call at any point; when
    // called mid-frame it sees the content of the last submitted frame (the
    // current pass is not flushed to the GPU yet — see docs/gx.md §J).
    // Returns false on failure (invalid handle, uninitialized renderer,
    // out-of-range region, allocation failure).
    bool readRenderTarget(RenderTargetHandle target, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, void* outRgba8);

    // --- pipeline cache -----------------------------------------------------
    // Returns a cached pipeline for `desc` (hash key); identical descs share
    // one Vulkan pipeline. Shaders are compiled at first use.
    PipelineHandle getOrCreatePipeline(const PipelineDesc& desc);
    void bindPipeline(PipelineHandle pipeline);

    // --- uniforms (M4.1: push constants, vertex stage) ----------------------
    // Binds a small per-draw uniform block (≤ 128 bytes). compat/gx packs
    // matrices/colors here; the demo pushes the 2D rotation matrix.
    void setUniforms(const void* data, uint32_t size);

    // M5.4 (TEV): uploads `size` bytes into the per-frame fragment-UBO arena
    // and binds the current pipeline's set 1 (UBO, dynamic offset) so the
    // fragment shader reads this draw's constants. Call after bindPipeline.
    // Returns false when the arena is exhausted (draw dropped by the caller).
    bool uploadFragmentUbo(const void* data, uint32_t size);

    // --- draw ---------------------------------------------------------------
    void draw(uint32_t vertexCount, uint32_t firstVertex);
    void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset);

    // Color format of the current pass attachment (the swapchain image for
    // M4.1). Pipelines must be created with this as PipelineDesc::colorFormat.
    VertexFormat passColorFormat() const;

    // Depth format of the current pass attachment (TextureFormat::Undefined
    // when the pass has no depth — e.g. the swapchain pass). compat/gx copies
    // this into PipelineDesc::depthFormat so depth-capable GX state doesn't
    // produce pipelines that declare depth on a depth-less pass.
    TextureFormat passDepthFormat() const;

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
    bool mFrameRecording = false;        // M5.7c: between beginFrame() and endFrame()
    bool mFrameAcquireConsumed = false;  // M5.7c: flushFrame() already waited on
                                         // the image-available semaphore
    bool mSwapPresentReady = false;      // swap image transitioned to
                                         // PRESENT_SRC this frame (endPass /
                                         // blitPassToSwapchain); endFrame adds
                                         // a fallback barrier for empty frames
    uint32_t mExtentW = 0;
    uint32_t mExtentH = 0;

    void* mCommandPool = nullptr;        // VkCommandPool (persistent, M4.1)
    void* mCmd = nullptr;                // VkCommandBuffer (current frame)
    void* mPassImageView = nullptr;      // VkImageView of the acquired swapchain image
    void* mDescriptorPool = nullptr;     // VkDescriptorPool (M4.2, textured pipelines)
    void* mFrameTexSetPool = nullptr;    // VkDescriptorPool (M9.5.3c: per-draw texture
                                         // sets; reset in endFrame after the frame fence)
    // M5.4 (TEV): per-frame fragment-UBO arena (host-visible, one region per
    // draw via dynamic offsets). Reset every endFrame() after the fence.
    void* mUboBuffer = nullptr;          // VkBuffer
    void* mUboMemory = nullptr;          // VkDeviceMemory (host visible + coherent)
    uint8_t* mUboMapped = nullptr;       // persistent map
    uint64_t mUboSize = 0;               // arena bytes
    uint64_t mUboCursor = 0;             // next free byte (aligned)
    uint64_t mUboStride = 0;             // per-draw region bytes (>= UBO size)
    uint32_t mUboAlign = 256;            // minUniformBufferOffsetAlignment
    void* mPassTarget = nullptr;         // GpuRenderTarget* of the current pass, or nullptr (swapchain)
    void* mFence = nullptr;              // VkFence (frame in flight)
    void* mImageAvailable = nullptr;     // VkSemaphore (acquire -> submit)
    std::vector<void*> mRenderFinished;  // VkSemaphore[imageCount] (submit -> present)

    float mClearR = 0.1f, mClearG = 0.1f, mClearB = 0.15f, mClearA = 1.0f;

    // M5.2: old allocations retired by ensureBufferCapacity, destroyed at the
    // next endFrame() after the frame fence is signalled (draws recorded this
    // frame may still reference them).
    struct RetiredBuffer {
        void* buffer;
        void* memory;
    };
    std::vector<RetiredBuffer> mRetiredBuffers;

    // M9.4: render targets retired by destroyRenderTarget(), destroyed at the
    // next endFrame() after the frame fence (or at shutdown). GX may resize
    // the EFB mid-frame (compat/gx ensureEfb recreates it when
    // GXSetDispCopySrc changes) while the blit recorded in the same frame
    // still references the old image AND reads the target struct through
    // mPassTarget — immediate destruction frees memory the GPU (and the blit
    // recording) still use.
    std::vector<void*> mRetiredRenderTargets;

    // M9.5.3c: textures retired by destroyTexture(), destroyed at the next
    // endFrame() after the frame fence (or at shutdown). The game destroys
    // textures MID-FRAME (nw4r re-inits texture objects from a shared stack
    // slot every draw; RLTP / TexMap::ReplaceImage swaps) while recorded
    // draws of the same frame still bind their image view — an immediate
    // vkDestroyImageView INVALIDATES the whole command buffer and every draw
    // of the frame silently disappears (the boot's black screen).
    std::vector<void*> mRetiredTextures;

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
        void* layout;                // VkPipelineLayout (push constants + set 0 + set 1)
        void* descriptorSetLayout;   // VkDescriptorSetLayout (textured pipelines only)
        void* descriptorSet;         // VkDescriptorSet (one per textured pipeline)
        void* uboSetLayout;          // VkDescriptorSetLayout (fragmentUbo pipelines only)
        void* uboSet;                // VkDescriptorSet (UBO, one per fragmentUbo pipeline)
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
