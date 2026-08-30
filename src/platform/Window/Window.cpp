// =============================================================================
// Platform::Window — SDL3 window (see Window.h).
// =============================================================================

#include "platform/Window/Window.h"

#include "platform/Log/Log.h"

#include <SDL3/SDL.h>

namespace Platform {

namespace {
// SDL_Init is process-wide; guard it so the first Window inits and the last
// one cleans up (only one window exists in practice).
int gWindowCount = 0;
} // namespace

Window::Window(const WindowConfig& config) {
    if (gWindowCount == 0) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            PL_LOG_FATAL("window", "SDL_Init failed: %s", SDL_GetError());
            return;
        }
        PL_LOG_INFO("window", "SDL initialized (video+events)");
    }
    ++gWindowCount;

    // SDL_WINDOW_VULKAN: the window must be flagged before SDL_Vulkan_CreateSurface
    // will accept it (SDL3 keeps this flag; without it CreateSurface fails with
    // "The specified window isn't a Vulkan window").
    Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    mWindow = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!mWindow) {
        PL_LOG_FATAL("window", "SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }
    if (config.startFullscreen) {
        SDL_SetWindowFullscreen(mWindow, SDL_WINDOW_FULLSCREEN);
        mFullscreen = true;
    }
    PL_LOG_INFO("window", "window '%s' %dx%d", config.title.c_str(), config.width, config.height);
}

Window::~Window() {
    if (mWindow) {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }
    if (--gWindowCount == 0) {
        SDL_Quit();
    }
}

int Window::width() const {
    int w = 0;
    if (mWindow) {
        SDL_GetWindowSize(mWindow, &w, nullptr);
    }
    return w;
}

int Window::height() const {
    int h = 0;
    if (mWindow) {
        SDL_GetWindowSize(mWindow, nullptr, &h);
    }
    return h;
}

void Window::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                mShouldClose = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                mShouldClose = true;
                break;
            default:
                break;
        }
    }
}

void Window::toggleFullscreen() {
    if (!mWindow) {
        return;
    }
    mFullscreen = !mFullscreen;
    if (!SDL_SetWindowFullscreen(mWindow, mFullscreen ? SDL_WINDOW_FULLSCREEN : 0)) {
        PL_LOG_WARN("window", "SDL_SetWindowFullscreen failed: %s", SDL_GetError());
        mFullscreen = !mFullscreen;
    }
}

bool Window::isFullscreen() const {
    return mFullscreen;
}

} // namespace Platform
