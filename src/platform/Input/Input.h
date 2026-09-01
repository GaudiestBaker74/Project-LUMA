#pragma once
// =============================================================================
// Platform::Input — full input layer (M6).
//
// Polls SDL3 events (keyboard, mouse, gamepads) and produces a raw per-frame
// snapshot. This is the *raw device* layer: it knows nothing about Wiimotes
// or mappings. The compat layer (src/compat/kpad + src/compat/wpad) turns the
// snapshot into KPAD/WPAD semantics (docs/input.md §2-3).
//
// Scope history: M3 seed (quit/fullscreen + a few keys) → M6 full input
// (keyboard/mouse/gamepad + rumble). The M3 flags (quit, fullscreenToggle,
// keyUp..keySpace) are kept for the demo main loop.
// =============================================================================

#include "platform/Window/Window.h"

#include <cstdint>

namespace Platform {

// Curated, SDL-free key/mouse-button codes (mapped from SDL scancodes in
// Input.cpp; named in config files via keyName()).
enum class Key : uint16_t {
    None = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Up, Down, Left, Right,
    Space, Enter, Escape, Backspace, Tab,
    ShiftL, ShiftR, CtrlL, CtrlR, AltL, AltR,
    Minus, Equals, LBracket, RBracket, Semicolon, Apostrophe, Comma, Period,
    Slash, Backquote,
    MouseLeft, MouseRight, MouseMiddle,
    Count
};

// Config-file name of a key ("a", "space", "mouse_left", ...). Returns nullptr
// for Key::None. Used by the input config parser (compat layer).
const char* keyName(Key key);
// Inverse of keyName(); returns Key::None for unknown names.
Key keyFromName(const char* name);

constexpr int kMaxGamepads = 4;

// Snapshot of one SDL gamepad (already mapped to the standard SDL layout, so
// it is brand-independent). Axes are in [-1,1] (deadzone applied in
// Input.cpp); triggers in [0,1].
struct GamepadState {
    bool connected = false;
    bool a = false;          // south
    bool b = false;          // east
    bool x = false;          // west
    bool y = false;          // north
    bool back = false;
    bool guide = false;
    bool start = false;
    bool leftStickBtn = false;
    bool rightStickBtn = false;
    bool leftShoulder = false;
    bool rightShoulder = false;
    bool dpadUp = false;
    bool dpadDown = false;
    bool dpadLeft = false;
    bool dpadRight = false;
    bool misc1 = false;
    float leftX = 0.0f;      // -1..1
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f; // 0..1
    float rightTrigger = 0.0f;
};

// Raw per-frame snapshot. Held-state only: edges (press/release) are derived
// by the consumers (the KPAD layer keeps its own previous-state).
struct InputState {
    bool quit = false;            // user asked to close (close button / ESC)
    bool fullscreenToggle = false; // F11 pressed this frame (consume once)

    // Legacy M3 convenience flags (arrows + space), kept for the demo loop.
    bool keyUp = false;
    bool keyDown = false;
    bool keyLeft = false;
    bool keyRight = false;
    bool keySpace = false;

    // Keyboard/mouse held state.
    bool keys[static_cast<int>(Key::Count)] = {};
    int mouseX = 0;               // window pixels, y down
    int mouseY = 0;
    int mouseDX = 0;              // delta since last poll
    int mouseDY = 0;
    float mouseNx = 0.5f;         // normalized [0,1], y UP (0 = top)
    float mouseNy = 0.5f;
    bool mouseInWindow = true;
    bool mouseLeft = false;
    bool mouseRight = false;
    bool mouseMiddle = false;

    GamepadState gamepads[kMaxGamepads] = {};
};

class Input {
public:
    // Polls the window's SDL events and returns the raw state for this frame.
    // Also updates the window's internal close flag.
    InputState poll(Window& window);

    // Motor control: gamepadIndex is a slot in [0, kMaxGamepads) that maps to
    // the i-th connected gamepad. No-op when no gamepad is present (or SDL is
    // unavailable, e.g. headless tests).
    void setRumble(int gamepadIndex, bool on);

    // Closes any open gamepads (called on shutdown).
    void shutdown();

private:
    // Open SDL_Gamepad* per slot (nullptr when empty) + the instance id that
    // occupies the slot (0 = empty). Stored as void*/u32 so this header does
    // not depend on SDL3.
    void* mGamepads[kMaxGamepads] = {};
    uint32_t mGamepadIds[kMaxGamepads] = {};
};

} // namespace Platform
