// =============================================================================
// Platform::Input — minimal SDL3 input (see Input.h).
// =============================================================================

#include "platform/Input/Input.h"

#include <SDL3/SDL.h>

namespace Platform {

InputState Input::poll(Window& window) {
    InputState state;

    // Edge-triggered flags come from the event queue.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                state.quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat) {
                    break;
                }
                switch (event.key.key) {
                    case SDLK_ESCAPE:
                        state.quit = true;
                        break;
                    case SDLK_F11:
                        state.fullscreenToggle = true;
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    // Level-triggered keyboard state (live for this frame).
    const bool* keys = SDL_GetKeyboardState(nullptr);
    state.keyUp = keys[SDL_SCANCODE_UP];
    state.keyDown = keys[SDL_SCANCODE_DOWN];
    state.keyLeft = keys[SDL_SCANCODE_LEFT];
    state.keyRight = keys[SDL_SCANCODE_RIGHT];
    state.keySpace = keys[SDL_SCANCODE_SPACE];

    return state;
}

} // namespace Platform
