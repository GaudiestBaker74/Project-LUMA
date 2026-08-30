#pragma once
// =============================================================================
// Platform::Window — SDL3 window abstraction.
//
// SDL3 abstracts the OS (Win32 / X11 / Wayland), so this module is
// platform-agnostic: no #ifdef here. The Vulkan surface is created by
// Platform::Video from the SDL window handle (SDL_Vulkan_CreateSurface).
// =============================================================================

#include <string>

struct SDL_Window;

namespace Platform {

struct WindowConfig {
    std::string title = "galaxy-pc";
    int width = 1280;
    int height = 720;
    bool resizable = true;
    bool startFullscreen = false;
};

class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // SDL window handle (owned here; Video uses it for the Vulkan surface).
    SDL_Window* handle() const { return mWindow; }

    int width() const;
    int height() const;

    // True after the user asked to close (window close button or OS quit).
    bool shouldClose() const { return mShouldClose; }

    // Pumps the SDL event queue, updating internal state (close flag). Event
    // details are consumed by Platform::Input; Window only tracks lifecycle.
    void pollEvents();

    void toggleFullscreen();
    bool isFullscreen() const;

private:
    SDL_Window* mWindow = nullptr;
    bool mShouldClose = false;
    bool mFullscreen = false;
};

} // namespace Platform
