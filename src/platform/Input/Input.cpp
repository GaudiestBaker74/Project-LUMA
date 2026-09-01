// =============================================================================
// Platform::Input — SDL3 implementation (see Input.h).
// =============================================================================

#include "platform/Input/Input.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>

namespace Platform {

// ---------------------------------------------------------------------------
// Key names + SDL scancode mapping.
// ---------------------------------------------------------------------------

struct KeyName {
    const char* name;
    Key key;
};

static const KeyName kKeyNames[] = {
    {"a", Key::A},       {"b", Key::B},       {"c", Key::C},       {"d", Key::D},
    {"e", Key::E},       {"f", Key::F},       {"g", Key::G},       {"h", Key::H},
    {"i", Key::I},       {"j", Key::J},       {"k", Key::K},       {"l", Key::L},
    {"m", Key::M},       {"n", Key::N},       {"o", Key::O},       {"p", Key::P},
    {"q", Key::Q},       {"r", Key::R},       {"s", Key::S},       {"t", Key::T},
    {"u", Key::U},       {"v", Key::V},       {"w", Key::W},       {"x", Key::X},
    {"y", Key::Y},       {"z", Key::Z},
    {"0", Key::D0},      {"1", Key::D1},      {"2", Key::D2},      {"3", Key::D3},
    {"4", Key::D4},      {"5", Key::D5},      {"6", Key::D6},      {"7", Key::D7},
    {"8", Key::D8},      {"9", Key::D9},
    {"f1", Key::F1},     {"f2", Key::F2},     {"f3", Key::F3},     {"f4", Key::F4},
    {"f5", Key::F5},     {"f6", Key::F6},     {"f7", Key::F7},     {"f8", Key::F8},
    {"f9", Key::F9},     {"f10", Key::F10},   {"f11", Key::F11},   {"f12", Key::F12},
    {"up", Key::Up},     {"down", Key::Down}, {"left", Key::Left}, {"right", Key::Right},
    {"space", Key::Space},     {"enter", Key::Enter},   {"escape", Key::Escape},
    {"backspace", Key::Backspace}, {"tab", Key::Tab},
    {"lshift", Key::ShiftL}, {"rshift", Key::ShiftR},
    {"lctrl", Key::CtrlL},    {"rctrl", Key::CtrlR},
    {"lalt", Key::AltL},      {"ralt", Key::AltR},
    {"minus", Key::Minus},     {"equals", Key::Equals},
    {"lbracket", Key::LBracket}, {"rbracket", Key::RBracket},
    {"semicolon", Key::Semicolon}, {"apostrophe", Key::Apostrophe},
    {"comma", Key::Comma},     {"period", Key::Period},
    {"slash", Key::Slash},     {"backquote", Key::Backquote},
    {"mouse_left", Key::MouseLeft}, {"mouse_right", Key::MouseRight},
    {"mouse_middle", Key::MouseMiddle},
};

const char* keyName(Key key) {
    for (const KeyName& entry : kKeyNames) {
        if (entry.key == key) {
            return entry.name;
        }
    }
    return nullptr;
}

Key keyFromName(const char* name) {
    if (name == nullptr) {
        return Key::None;
    }
    for (const KeyName& entry : kKeyNames) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.key;
        }
    }
    return Key::None;
}

static Key keyFromScancode(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_A: return Key::A;
        case SDL_SCANCODE_B: return Key::B;
        case SDL_SCANCODE_C: return Key::C;
        case SDL_SCANCODE_D: return Key::D;
        case SDL_SCANCODE_E: return Key::E;
        case SDL_SCANCODE_F: return Key::F;
        case SDL_SCANCODE_G: return Key::G;
        case SDL_SCANCODE_H: return Key::H;
        case SDL_SCANCODE_I: return Key::I;
        case SDL_SCANCODE_J: return Key::J;
        case SDL_SCANCODE_K: return Key::K;
        case SDL_SCANCODE_L: return Key::L;
        case SDL_SCANCODE_M: return Key::M;
        case SDL_SCANCODE_N: return Key::N;
        case SDL_SCANCODE_O: return Key::O;
        case SDL_SCANCODE_P: return Key::P;
        case SDL_SCANCODE_Q: return Key::Q;
        case SDL_SCANCODE_R: return Key::R;
        case SDL_SCANCODE_S: return Key::S;
        case SDL_SCANCODE_T: return Key::T;
        case SDL_SCANCODE_U: return Key::U;
        case SDL_SCANCODE_V: return Key::V;
        case SDL_SCANCODE_W: return Key::W;
        case SDL_SCANCODE_X: return Key::X;
        case SDL_SCANCODE_Y: return Key::Y;
        case SDL_SCANCODE_Z: return Key::Z;
        case SDL_SCANCODE_0: return Key::D0;
        case SDL_SCANCODE_1: return Key::D1;
        case SDL_SCANCODE_2: return Key::D2;
        case SDL_SCANCODE_3: return Key::D3;
        case SDL_SCANCODE_4: return Key::D4;
        case SDL_SCANCODE_5: return Key::D5;
        case SDL_SCANCODE_6: return Key::D6;
        case SDL_SCANCODE_7: return Key::D7;
        case SDL_SCANCODE_8: return Key::D8;
        case SDL_SCANCODE_9: return Key::D9;
        case SDL_SCANCODE_F1: return Key::F1;
        case SDL_SCANCODE_F2: return Key::F2;
        case SDL_SCANCODE_F3: return Key::F3;
        case SDL_SCANCODE_F4: return Key::F4;
        case SDL_SCANCODE_F5: return Key::F5;
        case SDL_SCANCODE_F6: return Key::F6;
        case SDL_SCANCODE_F7: return Key::F7;
        case SDL_SCANCODE_F8: return Key::F8;
        case SDL_SCANCODE_F9: return Key::F9;
        case SDL_SCANCODE_F10: return Key::F10;
        case SDL_SCANCODE_F11: return Key::F11;
        case SDL_SCANCODE_F12: return Key::F12;
        case SDL_SCANCODE_UP: return Key::Up;
        case SDL_SCANCODE_DOWN: return Key::Down;
        case SDL_SCANCODE_LEFT: return Key::Left;
        case SDL_SCANCODE_RIGHT: return Key::Right;
        case SDL_SCANCODE_SPACE: return Key::Space;
        case SDL_SCANCODE_RETURN: return Key::Enter;
        case SDL_SCANCODE_ESCAPE: return Key::Escape;
        case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
        case SDL_SCANCODE_TAB: return Key::Tab;
        case SDL_SCANCODE_LSHIFT: return Key::ShiftL;
        case SDL_SCANCODE_RSHIFT: return Key::ShiftR;
        case SDL_SCANCODE_LCTRL: return Key::CtrlL;
        case SDL_SCANCODE_RCTRL: return Key::CtrlR;
        case SDL_SCANCODE_LALT: return Key::AltL;
        case SDL_SCANCODE_RALT: return Key::AltR;
        case SDL_SCANCODE_MINUS: return Key::Minus;
        case SDL_SCANCODE_EQUALS: return Key::Equals;
        case SDL_SCANCODE_LEFTBRACKET: return Key::LBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return Key::RBracket;
        case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
        case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
        case SDL_SCANCODE_COMMA: return Key::Comma;
        case SDL_SCANCODE_PERIOD: return Key::Period;
        case SDL_SCANCODE_SLASH: return Key::Slash;
        case SDL_SCANCODE_GRAVE: return Key::Backquote;
        default: return Key::None;
    }
}

// ---------------------------------------------------------------------------
// Gamepads.
// ---------------------------------------------------------------------------

// Linear deadzone remap: |v| < dz → 0, otherwise rescale to [0,1].
static float deadzone(float v, float dz) {
    float magnitude = v < 0.0f ? -v : v;
    if (magnitude < dz) {
        return 0.0f;
    }
    float scaled = (magnitude - dz) / (1.0f - dz);
    return v < 0.0f ? -scaled : scaled;
}

static void readGamepad(void* handle, GamepadState& out) {
    SDL_Gamepad* pad = static_cast<SDL_Gamepad*>(handle);
    out.connected = SDL_GamepadConnected(pad);
    if (!out.connected) {
        return;
    }
    out.a = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    out.b = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
    out.x = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    out.y = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
    out.back = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
    out.guide = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
    out.start = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    out.leftStickBtn = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    out.rightStickBtn = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    out.leftShoulder = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    out.rightShoulder = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    out.dpadUp = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    out.dpadDown = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    out.dpadLeft = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    out.dpadRight = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    out.misc1 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_MISC1);
    out.leftX = deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f, 0.15f);
    out.leftY = deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f, 0.15f);
    out.rightX = deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f, 0.15f);
    out.rightY = deadzone(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f, 0.15f);
    out.leftTrigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    out.rightTrigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
}

// Finds the slot occupied by `id` (or the first free slot for `id == 0`).
static int findGamepadSlot(void* slots[kMaxGamepads], uint32_t ids[kMaxGamepads], uint32_t id) {
    for (int i = 0; i < kMaxGamepads; ++i) {
        if (ids[i] == id && id != 0) {
            return i;
        }
    }
    for (int i = 0; i < kMaxGamepads; ++i) {
        if (slots[i] == nullptr) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Input.
// ---------------------------------------------------------------------------

InputState Input::poll(Window& window) {
    InputState state;

    int prevMouseX = state.mouseX;
    int prevMouseY = state.mouseY;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                state.quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat) {
                    break; // ignore OS auto-repeat; edges derived per-frame
                }
                state.keys[static_cast<int>(keyFromScancode(event.key.scancode))] = true;
                break;
            case SDL_EVENT_KEY_UP:
                state.keys[static_cast<int>(keyFromScancode(event.key.scancode))] = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                state.mouseX = event.motion.x;
                state.mouseY = event.motion.y;
                state.mouseInWindow = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                switch (event.button.button) {
                    case SDL_BUTTON_LEFT: state.mouseLeft = down; break;
                    case SDL_BUTTON_RIGHT: state.mouseRight = down; break;
                    case SDL_BUTTON_MIDDLE: state.mouseMiddle = down; break;
                    default: break;
                }
                break;
            }
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                state.mouseInWindow = false;
                break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                state.mouseInWindow = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                const int slot = findGamepadSlot(mGamepads, mGamepadIds, 0);
                if (slot >= 0) {
                    SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which);
                    if (pad != nullptr) {
                        mGamepads[slot] = static_cast<void*>(pad);
                        mGamepadIds[slot] = event.gdevice.which;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                const int slot = findGamepadSlot(mGamepads, mGamepadIds, event.gdevice.which);
                if (slot >= 0) {
                    SDL_CloseGamepad(static_cast<SDL_Gamepad*>(mGamepads[slot]));
                    mGamepads[slot] = nullptr;
                    mGamepadIds[slot] = 0;
                }
                break;
            }
            default:
                break;
        }
    }

    // ESC quits, F11 toggles fullscreen (M3 behavior).
    if (state.keys[static_cast<int>(Key::Escape)]) {
        state.quit = true;
    }
    if (state.keys[static_cast<int>(Key::F11)]) {
        state.fullscreenToggle = true;
    }

    // Legacy arrow/space flags.
    state.keyUp = state.keys[static_cast<int>(Key::Up)];
    state.keyDown = state.keys[static_cast<int>(Key::Down)];
    state.keyLeft = state.keys[static_cast<int>(Key::Left)];
    state.keyRight = state.keys[static_cast<int>(Key::Right)];
    state.keySpace = state.keys[static_cast<int>(Key::Space)];

    // Mouse deltas (events accumulate xrel/yrel, so derive from position
    // deltas — robust against mouse warps on fullscreen toggles).
    state.mouseDX = state.mouseX - prevMouseX;
    state.mouseDY = state.mouseY - prevMouseY;

    // Normalized mouse position (y up), for the KPAD pointer mapping.
    const int winW = window.width();
    const int winH = window.height();
    if (winW > 0 && winH > 0) {
        state.mouseNx = (state.mouseX + 0.5f) / winW;
        state.mouseNy = 1.0f - (state.mouseY + 0.5f) / winH;
    }

    // Gamepads already connected before the first poll (added before we
    // started draining events).
    if (mGamepads[0] == nullptr && mGamepads[1] == nullptr && mGamepads[2] == nullptr && mGamepads[3] == nullptr) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count && i < kMaxGamepads; ++i) {
            mGamepads[i] = static_cast<void*>(SDL_OpenGamepad(ids[i]));
            mGamepadIds[i] = ids[i];
        }
    }

    for (int i = 0; i < kMaxGamepads; ++i) {
        if (mGamepads[i] != nullptr) {
            readGamepad(mGamepads[i], state.gamepads[i]);
        }
    }

    return state;
}

void Input::setRumble(int gamepadIndex, bool on) {
    if (gamepadIndex < 0 || gamepadIndex >= kMaxGamepads) {
        return;
    }
    SDL_Gamepad* pad = static_cast<SDL_Gamepad*>(mGamepads[gamepadIndex]);
    if (pad == nullptr || !SDL_GamepadConnected(pad)) {
        return;
    }
    // duration 0 = rumble until stopped.
    const Uint16 strength = on ? 0xFFFFu : 0u;
    SDL_RumbleGamepad(pad, strength, strength, 0);
}

void Input::shutdown() {
    for (int i = 0; i < kMaxGamepads; ++i) {
        if (mGamepads[i] != nullptr) {
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(mGamepads[i]));
            mGamepads[i] = nullptr;
            mGamepadIds[i] = 0;
        }
    }
}

} // namespace Platform
