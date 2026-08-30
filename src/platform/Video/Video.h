#pragma once
// =============================================================================
// Platform::Video — windowed Vulkan backend (Milestone 3 scope).
//
// Owns the Vulkan instance, device, swapchain and the present path. The full
// Platform::Renderer API (resources, pipeline cache, uniforms...) is Milestone
// 4 (see docs/renderer.md); this module only provides what M3 needs:
//
//   * instance + physical device + logical device (with dynamic rendering),
//   * swapchain with vsync (FIFO), recreated on resize/out-of-date,
//   * a single demo pipeline (embedded SPIR-V triangle) with a vertex buffer,
//   * begin/end frame with presentation and correct delta-driven rendering,
//   * validation layers + debug labels when enabled (--gpu-debug).
//
// The compat/gx module (M3 "first GX smell") drives the clear color and the
// viewport through this API; main.cpp runs the fixed-timestep game loop.
// =============================================================================

#include <cstdint>
#include <vector>

struct SDL_Window;

namespace Platform {

struct VideoConfig {
    const char* appName = "galaxy-pc";
    bool enableValidation = false; // VK_LAYER_KHRONOS_validation (+ debug utils)
    bool vsync = true;             // VK_PRESENT_MODE_FIFO_KHR
    int swapchainWidth = 0;        // 0 = window size at init
    int swapchainHeight = 0;
};

class Video {
public:
    // --- lifecycle ----------------------------------------------------------
    // Requires an existing SDL window (Platform::Window). Returns false on
    // fatal setup failure (no Vulkan loader/device). Idempotent after
    // shutdown().
    static bool init(SDL_Window* window, const VideoConfig& config);
    static void shutdown();
    static Video& instance();

    bool isInitialized() const { return mDevice != nullptr; }
    bool validationEnabled() const { return mValidation; }

    // --- frame --------------------------------------------------------------
    // Acquires the next swapchain image. Returns false when the swapchain was
    // recreated (caller should retry the frame or skip rendering this tick).
    bool beginFrame();
    // Renders the M3 demo: clears with the current GX clear color, applies the
    // current GX viewport, draws the embedded triangle.
    void renderDemo(float dtSeconds);
    // Presents. Handles VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR by
    // recreating the swapchain. Returns false if the window was destroyed.
    bool endFrame();
    void waitIdle();

    // --- GX hooks (driven by compat/gx; M3 scope) ----------------------------
    void setClearColor(float r, float g, float b, float a);
    void setViewport(float x, float y, float w, float h);

    // M3 demo: rotation angle (radians) applied to the demo triangle, updated
    // by the fixed-timestep loop so the rotation visibly follows the 60 Hz
    // step.
    void setDemoAngle(float radians);

    // Called by the window resize path.
    void onResize(int width, int height);

private:
    Video() = default;
    ~Video() = default;
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    bool createInstance(SDL_Window* window);
    bool createDeviceAndSwapchain(SDL_Window* window);
    void destroySwapchain();
    void recreateSwapchain(SDL_Window* window);
    bool createDemoPipeline();
    void destroyDemoPipeline();

    // --- Vulkan state -------------------------------------------------------
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
    std::uint32_t mExtentW = 0;
    std::uint32_t mExtentH = 0;

    // demo pipeline state
    void* mPipeline = nullptr;           // VkPipeline
    void* mPipelineLayout = nullptr;     // VkPipelineLayout
    void* mVertexBuffer = nullptr;       // VkBuffer
    void* mVertexMemory = nullptr;       // VkDeviceMemory
    void* mRenderPass = nullptr;         // VkRenderPass (null — dynamic rendering)
    void* mFence = nullptr;              // VkFence (frame in flight)
    void* mImageAvailable = nullptr;     // VkSemaphore (acquire -> submit)
    // One render-finished semaphore per swapchain image: a binary semaphore
    // must not be re-signaled while the previous present is still consuming
    // it, and the swapchain can hand an image back for re-acquire only after
    // its present completed (validation VUID-vkQueueSubmit-pSignalSemaphores-00067).
    // Lifespan == swapchain lifespan (recreated with it).
    std::vector<void*> mRenderFinished;  // VkSemaphore[imageCount] (submit -> present)

    float mDemoAngle = 0.0f;

    float mClearR = 0.1f, mClearG = 0.1f, mClearB = 0.15f, mClearA = 1.0f;
    float mViewportX = 0, mViewportY = 0, mViewportW = 0, mViewportH = 0;
    bool mViewportSet = false;
    bool mValidation = false;
    bool mVsync = true;
    bool mSwapchainOutOfDate = false;
};

} // namespace Platform
