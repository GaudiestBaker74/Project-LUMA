#pragma once
// =============================================================================
// Platform::Input — minimal input for M3.
//
// Polls SDL3 events for a window and produces a small per-frame InputState.
// Scope for M3: quit, fullscreen toggle, and a handful of keys (ESC + a few
// movement keys). The full Wiimote→PC mapping is Milestone 6 (porting.md §5);
// this module is its seed.
// =============================================================================

#include "platform/Window/Window.h"

namespace Platform {

struct InputState {
    bool quit = false;          // user asked to close (close button / ESC)
    bool fullscreenToggle = false; // F11 pressed this frame (consume it once)
    bool keyUp = false;
    bool keyDown = false;
    bool keyLeft = false;
    bool keyRight = false;
    bool keySpace = false;
};

class Input {
public:
    // Polls the window's SDL events and returns the state for this frame.
    // Also updates the window's internal close flag.
    InputState poll(Window& window);
};

} // namespace Platform
